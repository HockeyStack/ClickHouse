#pragma once

#include <Core/Names.h>
#include <Core/NamesAndTypes.h>
#include <Interpreters/Context_fwd.h>
#include <Interpreters/TreeRewriter.h>
#include <Interpreters/replaceSubcolumnsToGetSubcolumnFunctionInQuery.h>
#include <Storages/ColumnsDescription.h>
#include <base/types.h>
#include <Common/Exception.h>

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

using MaterializedColumnDependencies = std::unordered_map<String, NameSet>;

struct MaterializedColumnDependencyOptions
{
    const NameSet * available_columns = nullptr;
    const NameSet * ephemeral_columns = nullptr;
    const NameSet * changed_columns_for_warning = nullptr;
    std::function<void(const String &)> warn_about_skipped_changed_column;
};

inline MaterializedColumnDependencies buildMaterializedColumnDependencies(
    const ColumnsDescription & columns_desc,
    const NamesAndTypesList & all_columns_with_ephemeral,
    const ContextPtr & context,
    const MaterializedColumnDependencyOptions & options = {})
{
    MaterializedColumnDependencies materialized_dependencies;
    const NameSet empty_ephemeral_columns;
    const auto & ephemeral_columns = options.ephemeral_columns ? *options.ephemeral_columns : empty_ephemeral_columns;

    for (const auto & column : columns_desc)
    {
        if (column.default_desc.kind != ColumnDefaultKind::Materialized
            || (options.available_columns && !options.available_columns->contains(column.name))
            || !column.default_desc.expression)
            continue;

        auto query = column.default_desc.expression->clone();
        replaceSubcolumnsToGetSubcolumnFunctionInQuery(query, all_columns_with_ephemeral);
        auto syntax_result = TreeRewriter(context).analyze(query, all_columns_with_ephemeral);
        auto required_columns = syntax_result->requiredSourceColumns();

        /// If the `MATERIALIZED` expression depends on any `EPHEMERAL` column,
        /// skip it: `EPHEMERAL` columns are only available during `INSERT`
        /// and cannot be read from disk during mutations.
        if (std::ranges::any_of(required_columns,
            [&](const auto & dependency)
            {
                return ephemeral_columns.contains(dependency);
            }))
        {
            /// Warn if the mutation also changes a non-`EPHEMERAL` dependency
            /// of this `MATERIALIZED` column. The on-disk value will become stale.
            if (options.changed_columns_for_warning
                && options.warn_about_skipped_changed_column
                && std::ranges::any_of(required_columns,
                    [&](const auto & dependency)
                    {
                        return !ephemeral_columns.contains(dependency) && options.changed_columns_for_warning->contains(dependency);
                    }))
                options.warn_about_skipped_changed_column(column.name);

            continue;
        }

        auto & dependencies = materialized_dependencies[column.name];
        for (const auto & dependency : required_columns)
            dependencies.insert(dependency);
    }

    return materialized_dependencies;
}

inline NameSet getAffectedMaterializedColumns(
    const MaterializedColumnDependencies & materialized_dependencies,
    const NameSet & initially_changed_columns)
{
    NameSet affected;
    NameSet changed_columns = initially_changed_columns;

    while (true)
    {
        bool has_new_affected_column = false;

        for (const auto & [materialized_column, dependencies] : materialized_dependencies)
        {
            if (affected.contains(materialized_column))
                continue;

            const bool depends_on_changed_column = std::ranges::any_of(dependencies,
                [&](const auto & dependency)
                {
                    return changed_columns.contains(dependency);
                });

            if (depends_on_changed_column)
            {
                affected.insert(materialized_column);
                changed_columns.insert(materialized_column);
                has_new_affected_column = true;
            }
        }

        if (!has_new_affected_column)
            break;
    }

    return affected;
}

inline std::vector<NameSet> getAffectedMaterializedBatches(
    const MaterializedColumnDependencies & materialized_dependencies,
    const NameSet & initially_changed_columns)
{
    auto affected = getAffectedMaterializedColumns(materialized_dependencies, initially_changed_columns);
    std::vector<NameSet> batches;
    NameSet already_scheduled;

    while (already_scheduled.size() != affected.size())
    {
        NameSet batch;

        for (const auto & materialized_column : affected)
        {
            if (already_scheduled.contains(materialized_column))
                continue;

            const auto & dependencies = materialized_dependencies.at(materialized_column);
            const bool waits_for_affected_materialized_column = std::ranges::any_of(dependencies,
                [&](const auto & dependency)
                {
                    return affected.contains(dependency) && !already_scheduled.contains(dependency);
                });

            if (!waits_for_affected_materialized_column)
                batch.insert(materialized_column);
        }

        if (batch.empty())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Cyclic dependency between affected `MATERIALIZED` columns");

        for (const auto & column : batch)
            already_scheduled.insert(column);

        batches.push_back(std::move(batch));
    }

    return batches;
}

inline NameSet flattenMaterializedBatches(const std::vector<NameSet> & batches)
{
    NameSet result;
    for (const auto & batch : batches)
        result.insert(batch.begin(), batch.end());
    return result;
}

inline NameSet getPhysicalColumnNamesForClear(const ColumnsDescription & columns_desc, const String & column_name)
{
    NameSet cleared_column_names;
    if (columns_desc.has(column_name))
    {
        cleared_column_names.emplace(column_name);
    }
    else
    {
        const auto & nested = columns_desc.getNested(column_name);
        for (const auto & nested_column : nested)
            cleared_column_names.emplace(nested_column.name);
    }

    return cleared_column_names;
}

}
