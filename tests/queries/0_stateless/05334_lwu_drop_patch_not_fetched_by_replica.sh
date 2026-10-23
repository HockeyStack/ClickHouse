#!/usr/bin/env bash
# Tags: no-replicated-database, no-shared-catalog, no-shared-merge-tree

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -e

$CLICKHOUSE_CLIENT --query "
    DROP TABLE IF EXISTS t_lwu_drop_patch_1 SYNC;
    DROP TABLE IF EXISTS t_lwu_drop_patch_2 SYNC;

    CREATE TABLE t_lwu_drop_patch_1 (k UInt64, v String)
    ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/t_lwu_drop_patch', '1')
    PARTITION BY k % 2
    ORDER BY k
    SETTINGS
        enable_block_number_column = 1,
        enable_block_offset_column = 1,
        cleanup_delay_period = 1,
        max_cleanup_delay_period = 1,
        cleanup_delay_period_random_add = 0;

    CREATE TABLE t_lwu_drop_patch_2 (k UInt64, v String)
    ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/t_lwu_drop_patch', '2')
    PARTITION BY k % 2
    ORDER BY k
    SETTINGS
        enable_block_number_column = 1,
        enable_block_offset_column = 1,
        cleanup_delay_period = 1,
        max_cleanup_delay_period = 1,
        cleanup_delay_period_random_add = 0;

    SET enable_lightweight_update = 1;
    SET insert_keeper_fault_injection_probability = 0.0;

    INSERT INTO t_lwu_drop_patch_1 VALUES (1, 'v1') (2, 'v2') (3, 'v3') (4, 'v4');
    -- Give the mutation of partition 1 a data version above the data version of the patch part of partition 0.
    INSERT INTO t_lwu_drop_patch_1 VALUES (5, 'v5');
    OPTIMIZE TABLE t_lwu_drop_patch_1 PARTITION ID '1' FINAL;
    SYSTEM SYNC REPLICA t_lwu_drop_patch_2;
    SYSTEM STOP FETCHES t_lwu_drop_patch_2;
    SYSTEM STOP MERGES t_lwu_drop_patch_2;

    UPDATE t_lwu_drop_patch_1 SET v = 'u2' WHERE k = 2;
    ALTER TABLE t_lwu_drop_patch_1 UPDATE v = v || '_m' WHERE 1;
"

# Replica 1 applies the patch part of partition 0 in the mutation and drops it before replica 2 fetches it.
# Replica 2 must fetch the mutated part of partition 0, and mutate the part of partition 1 itself.
for _ in {0..60}; do
    res=$($CLICKHOUSE_CLIENT --query "
        SELECT count() FROM system.parts
        WHERE database = currentDatabase() AND table = 't_lwu_drop_patch_1' AND active AND startsWith(name, 'patch')")
    if [[ $res == "0" ]]; then
        break
    fi
    sleep 1.0
done

if [[ $res != "0" ]]; then
    echo "Replica 1 did not drop the patch part"
fi

$CLICKHOUSE_CLIENT --query "
    SYSTEM SYNC REPLICA t_lwu_drop_patch_2 PULL;
    SYSTEM START FETCHES t_lwu_drop_patch_2;
    SYSTEM START MERGES t_lwu_drop_patch_2;
    SYSTEM SYNC REPLICA t_lwu_drop_patch_2;

    SELECT _partition_id, * FROM t_lwu_drop_patch_1 ORDER BY k SETTINGS apply_patch_parts = 0;
    SELECT _partition_id, * FROM t_lwu_drop_patch_2 ORDER BY k SETTINGS apply_patch_parts = 0;

    SYSTEM FLUSH LOGS part_log;
    SELECT partition_id, event_type FROM system.part_log
    WHERE database = currentDatabase() AND table = 't_lwu_drop_patch_2'
        AND event_type IN ('MutatePart', 'DownloadPart') AND error = 0 AND match(part_name, '^\\\\d+_\\\\d+_\\\\d+_\\\\d+_\\\\d+$')
    ORDER BY partition_id;

    DROP TABLE t_lwu_drop_patch_1 SYNC;
    DROP TABLE t_lwu_drop_patch_2 SYNC;
"
