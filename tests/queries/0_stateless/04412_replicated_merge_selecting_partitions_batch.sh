#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# Tests the `replicated_merge_selecting_partitions_batch_size` setting: the background merge
# selecting task considers a bounded round-robin batch of partitions per run instead of scanning
# all of them. The round-robin cursor must cover every partition over successive runs, so no
# partition is starved of merges, and data must be preserved.

${CLICKHOUSE_CLIENT} -m -q "
DROP TABLE IF EXISTS t_rr_batch SYNC;

CREATE TABLE t_rr_batch (p UInt8, id UInt64, v UInt64)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/t_rr_batch', 'r1')
PARTITION BY p ORDER BY (p, id)
SETTINGS replicated_merge_selecting_partitions_batch_size = 2,
         merge_selecting_sleep_ms = 100,
         max_merge_selecting_sleep_ms = 500;

SYSTEM STOP MERGES t_rr_batch;
"

# 8 partitions, 6 parts each.
for i in 1 2 3 4 5 6; do
    ${CLICKHOUSE_CLIENT} -q "INSERT INTO t_rr_batch SELECT number % 8, number * 10 + $i, $i FROM numbers(8) SETTINGS max_partitions_per_insert_block = 16"
done

${CLICKHOUSE_CLIENT} -q "SYSTEM START MERGES t_rr_batch"

# Wait until the round-robin batch selection has merged (reduced the part count in) every partition.
for _ in {1..120}; do
    merged=$(${CLICKHOUSE_CLIENT} -q "
        SELECT count() FROM (
            SELECT partition_id, count() AS c FROM system.parts
            WHERE database = currentDatabase() AND table = 't_rr_batch' AND active
            GROUP BY partition_id HAVING c < 6)")
    [ "$merged" = "8" ] && break
    sleep 0.5
done

# Every partition was merged at least once (no starvation).
${CLICKHOUSE_CLIENT} -q "
SELECT count() = 8 FROM (
    SELECT partition_id, count() AS c FROM system.parts
    WHERE database = currentDatabase() AND table = 't_rr_batch' AND active
    GROUP BY partition_id HAVING c < 6)"

# Data is preserved.
${CLICKHOUSE_CLIENT} -q "SELECT count(), sum(v) FROM t_rr_batch"

${CLICKHOUSE_CLIENT} -q "DROP TABLE t_rr_batch SYNC"
