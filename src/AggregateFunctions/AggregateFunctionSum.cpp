#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/AggregateFunctionSum.h>
#include <AggregateFunctions/Helpers.h>
#include <AggregateFunctions/FactoryHelpers.h>
#include <DataTypes/getLeastSupertype.h>


namespace DB
{
struct Settings;

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

namespace
{

template <typename T>
struct SumSimple
{
    /// @note It uses slow Decimal128/256 (cause we need such a variant). sumWithOverflow is faster for Decimal32/64
    using ResultType = std::conditional_t<is_decimal<T>,
                                        std::conditional_t<std::is_same_v<T, Decimal256>, Decimal256, Decimal128>,
                                        NearestFieldType<T>>;
    using AggregateDataType = AggregateFunctionSumData<ResultType>;
    using Function = AggregateFunctionSum<T, ResultType, AggregateDataType, AggregateFunctionTypeSum>;
};

template <typename T>
struct SumSameType
{
    using ResultType = T;
    using AggregateDataType = AggregateFunctionSumData<ResultType>;
    using Function = AggregateFunctionSum<T, ResultType, AggregateDataType, AggregateFunctionTypeSumWithOverflow>;
};

template <typename T>
struct SumKahan
{
    using ResultType = Float64;
    using AggregateDataType = AggregateFunctionSumKahanData<ResultType>;
    using Function = AggregateFunctionSum<T, ResultType, AggregateDataType, AggregateFunctionTypeSumKahan>;
};

template <typename T, AggregateFunctionSumType Type>
struct SumNeumaier
{
    using ResultType = Float64;
    using AggregateDataType = AggregateFunctionSumNeumaierData<ResultType>;
    using Function = AggregateFunctionSum<T, ResultType, AggregateDataType, Type>;
};

template <typename T> using AggregateFunctionSumSimple = typename SumSimple<T>::Function;
template <typename T> using AggregateFunctionSumWithOverflow = typename SumSameType<T>::Function;
template <typename T> using AggregateFunctionSumKahan =
    std::conditional_t<is_decimal<T>, typename SumSimple<T>::Function, typename SumKahan<T>::Function>;
template <typename T> using AggregateFunctionSumNeumaier = std::conditional_t<is_decimal<T>,
    typename SumSimple<T>::Function, typename SumNeumaier<T, AggregateFunctionTypeSumNeumaier>::Function>;
template <typename T> using AggregateFunctionSumNeumaierHiLo = typename SumNeumaier<T, AggregateFunctionTypeSumNeumaierHiLo>::Function;


template <template <typename> class Function, bool supports_decimal = true>
AggregateFunctionPtr createAggregateFunctionSum(const std::string & name, const DataTypes & argument_types, const Array & parameters, const Settings *)
{
    assertNoParameters(name, parameters);
    assertUnary(name, argument_types);

    AggregateFunctionPtr res;
    const DataTypePtr & data_type = argument_types[0];
    if (isDecimal(data_type))
    {
        if constexpr (supports_decimal)
            res.reset(createWithDecimalType<Function>(*data_type, *data_type, argument_types));
    }
    else
        res.reset(createWithNumericType<Function>(*data_type, argument_types));

    if (!res)
        throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Illegal type {} of argument for aggregate function {}{}",
                        argument_types[0]->getName(), name, getNumericVariantSupertypeHint(argument_types[0]));
    return res;
}

}

void registerAggregateFunctionSum(AggregateFunctionFactory & factory);
void registerAggregateFunctionSum(AggregateFunctionFactory & factory)
{
    FunctionDocumentation::Description description = R"(
Calculates the sum of numeric values.
    )";
    FunctionDocumentation::Syntax syntax = R"(
sum(num)
    )";
    FunctionDocumentation::Arguments arguments = {
        {"num", "Column of numeric values.", {"(U)Int*", "Float*", "Decimal*"}}
    };
    FunctionDocumentation::ReturnedValue returned_value = {
        "Returns the sum of the values.", {"(U)Int*", "Float*", "Decimal*"}
    };
    FunctionDocumentation::Examples examples = {
    {
        "Computing sum of employee salaries",
        R"(
CREATE TABLE employees
(
    id UInt32,
    name String,
    salary UInt32
)
ENGINE = Memory;

INSERT INTO employees VALUES
    (87432, 'John Smith', 45680),
    (59018, 'Jane Smith', 72350),
    (20376, 'Ivan Ivanovich', 58900),
    (71245, 'Anastasia Ivanovna', 89210);

SELECT sum(salary) FROM employees;
        )",
        R"(
┌─sum(salary)─┐
│      266140 │
└─────────────┘
        )"
    }
    };
    FunctionDocumentation::IntroducedIn introduced_in = {1, 1};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::AggregateFunction;
    FunctionDocumentation documentation = {description, syntax, arguments, {}, returned_value, examples, introduced_in, category};

    factory.registerFunction("sum", {createAggregateFunctionSum<AggregateFunctionSumSimple>, documentation}, AggregateFunctionFactory::Case::Insensitive);

    FunctionDocumentation::Description description_overflow = R"(
Computes a sum of numeric values, using the same data type for the result as for the input parameters.
If the sum exceeds the maximum value for this data type, it is calculated with overflow.
    )";
    FunctionDocumentation::Syntax syntax_overflow = R"(
sumWithOverflow(num)
    )";
    FunctionDocumentation::Arguments arguments_overflow = {
        {"num", "Column of numeric values.", {"(U)Int*", "Float*", "Decimal*"}}
    };
    FunctionDocumentation::ReturnedValue returned_value_overflow = {
        "The sum of the values.", {"(U)Int*", "Float*", "Decimal*"}
    };
    FunctionDocumentation::Examples examples_overflow = {
    {
        "Demonstrating overflow behavior with UInt16",
        R"(
CREATE TABLE employees
(
    id UInt32,
    name String,
    monthly_salary UInt16 -- selected so that the sum of values produces an overflow
)
ENGINE = Memory;

INSERT INTO employees VALUES
    (1, 'John', 20000),
    (2, 'Jane', 18000),
    (3, 'Bob', 12000),
    (4, 'Alice', 10000),
    (5, 'Charlie', 8000);

-- Query for the total amount of the employee salaries using the sum and sumWithOverflow functions and show their types using the toTypeName function
-- For the sum function the resulting type is UInt64, big enough to contain the sum, whilst for sumWithOverflow the resulting type remains as UInt16.

SELECT
    sum(monthly_salary) AS no_overflow,
    sumWithOverflow(monthly_salary) AS overflow,
    toTypeName(no_overflow),
    toTypeName(overflow)
FROM employees;
        )",
        R"(
┌─no_overflow─┬─overflow─┬─toTypeName(no_overflow)─┬─toTypeName(overflow)─┐
│       68000 │     2464 │ UInt64                  │ UInt16               │
└─────────────┴──────────┴─────────────────────────┴──────────────────────┘
        )"
    }
    };
    FunctionDocumentation::IntroducedIn introduced_in_overflow = {1, 1};
    FunctionDocumentation::Category category_overflow = FunctionDocumentation::Category::AggregateFunction;
    FunctionDocumentation documentation_overflow = {description_overflow, syntax_overflow, arguments_overflow, {}, returned_value_overflow, examples_overflow, introduced_in_overflow, category_overflow};

    factory.registerFunction("sumWithOverflow", {createAggregateFunctionSum<AggregateFunctionSumWithOverflow>, documentation_overflow});

    FunctionDocumentation::Description description_kahan = R"(
Calculates the sum of the numbers with [Kahan compensated summation algorithm](https://en.wikipedia.org/wiki/Kahan_summation_algorithm).
Slower than [`sum`](/reference/functions/aggregate-functions/sum) function.
The compensation works only for [Float](/reference/data-types/float) types.
    )";
    FunctionDocumentation::Syntax syntax_kahan = R"(
sumKahan(x)
    )";
    FunctionDocumentation::Arguments arguments_kahan = {
        {"x", "Input value.", {"Integer", "Float", "Decimal"}}
    };
    FunctionDocumentation::ReturnedValue returned_value_kahan = {
        "Returns the sum of numbers.", {"(U)Int*", "Float*", "Decimal"}
    };
    FunctionDocumentation::Examples examples_kahan = {
    {
        "Demonstrating precision improvement with Kahan summation",
        R"(
SELECT sum(0.1), sumKahan(0.1) FROM numbers(10);
        )",
        R"(
┌───────────sum(0.1)─┬─sumKahan(0.1)─┐
│ 0.9999999999999999 │             1 │
└────────────────────┴───────────────┘
        )"
    }
    };
    FunctionDocumentation::IntroducedIn introduced_in_kahan = {1, 1};
    FunctionDocumentation::Category category_kahan = FunctionDocumentation::Category::AggregateFunction;
    FunctionDocumentation documentation_kahan = {description_kahan, syntax_kahan, arguments_kahan, {}, returned_value_kahan, examples_kahan, introduced_in_kahan, category_kahan};

    factory.registerFunction("sumKahan", {createAggregateFunctionSum<AggregateFunctionSumKahan>, documentation_kahan});

    FunctionDocumentation::Description description_neumaier = R"(
Calculates the sum of the numbers with the [Neumaier compensated summation algorithm](https://en.wikipedia.org/wiki/Kahan_summation_algorithm#Further_enhancements),
also known as Kahan-Babuska summation.
Unlike [`sumKahan`](/reference/functions/aggregate-functions/sumKahan), it keeps the compensation when a value is larger in magnitude than the running sum,
for example when large values cancel each other.
Slower than [`sum`](/reference/functions/aggregate-functions/sum) function.
The compensation works only for [Float](/reference/data-types/float) types.
    )";
    FunctionDocumentation::Syntax syntax_neumaier = R"(
sumNeumaier(x)
    )";
    FunctionDocumentation::Arguments arguments_neumaier = {
        {"x", "Input value.", {"Integer", "Float", "Decimal"}}
    };
    FunctionDocumentation::ReturnedValue returned_value_neumaier = {
        "Returns the sum of numbers.", {"Float64", "Decimal"}
    };
    FunctionDocumentation::Examples examples_neumaier = {
    {
        "Large values that cancel each other",
        R"(
SELECT sum(x), sumKahan(x), sumNeumaier(x) FROM values('x Float64', 1000, 1e-18, -1000);
        )",
        R"(
┌─sum(x)─┬─sumKahan(x)─┬─sumNeumaier(x)─┐
│      0 │           0 │          1e-18 │
└────────┴─────────────┴────────────────┘
        )"
    }
    };
    FunctionDocumentation::IntroducedIn introduced_in_neumaier = {26, 8};
    FunctionDocumentation::Category category_neumaier = FunctionDocumentation::Category::AggregateFunction;
    FunctionDocumentation documentation_neumaier = {description_neumaier, syntax_neumaier, arguments_neumaier, {}, returned_value_neumaier, examples_neumaier, introduced_in_neumaier, category_neumaier};

    factory.registerFunction("sumNeumaier", {createAggregateFunctionSum<AggregateFunctionSumNeumaier>, documentation_neumaier});

    FunctionDocumentation::Description description_neumaier_hi_lo = R"(
Calculates the sum of the numbers like [`sumNeumaier`](/reference/functions/aggregate-functions/sumNeumaier), and returns it as two parts:
`hi` is the result of `sumNeumaier`, and `lo` is the remainder that `hi` cannot hold.
The difference of two running sums keeps its precision when the parts are subtracted separately: `(a.hi - b.hi) + (a.lo - b.lo)`.
If the sum is not finite, `hi` is the sum and `lo` is zero.
    )";
    FunctionDocumentation::Syntax syntax_neumaier_hi_lo = R"(
sumNeumaierHiLo(x)
    )";
    FunctionDocumentation::Arguments arguments_neumaier_hi_lo = {
        {"x", "Input value.", {"(U)Int*", "Float*"}}
    };
    FunctionDocumentation::ReturnedValue returned_value_neumaier_hi_lo = {
        "Returns the sum of numbers as a tuple `(hi, lo)`.", {"Tuple(hi Float64, lo Float64)"}
    };
    FunctionDocumentation::Examples examples_neumaier_hi_lo = {
    {
        "A small window of a running sum after a large value",
        R"(
SELECT n, s.hi, s.lo, (s.hi - first_s.hi) + (s.lo - first_s.lo) AS since_first
FROM
(
    SELECT n, s, first_value(s) OVER (ORDER BY n) AS first_s
    FROM (SELECT n, sumNeumaierHiLo(x) OVER (ORDER BY n) AS s FROM values('n UInt8, x Float64', (1, 1e20), (2, 1), (3, 1)))
)
ORDER BY n;
        )",
        R"(
┌─n─┬──────────────────s.hi─┬─s.lo─┬─since_first─┐
│ 1 │ 100000000000000000000 │    0 │           0 │
│ 2 │ 100000000000000000000 │    1 │           1 │
│ 3 │ 100000000000000000000 │    2 │           2 │
└───┴───────────────────────┴──────┴─────────────┘
        )"
    }
    };
    FunctionDocumentation::IntroducedIn introduced_in_neumaier_hi_lo = {26, 8};
    FunctionDocumentation::Category category_neumaier_hi_lo = FunctionDocumentation::Category::AggregateFunction;
    FunctionDocumentation documentation_neumaier_hi_lo = {description_neumaier_hi_lo, syntax_neumaier_hi_lo, arguments_neumaier_hi_lo, {}, returned_value_neumaier_hi_lo, examples_neumaier_hi_lo, introduced_in_neumaier_hi_lo, category_neumaier_hi_lo};

    factory.registerFunction("sumNeumaierHiLo", {createAggregateFunctionSum<AggregateFunctionSumNeumaierHiLo, false>, documentation_neumaier_hi_lo});
}

}
