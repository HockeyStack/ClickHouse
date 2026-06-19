SET mutations_sync = 1;
SET allow_statistics = 1;
SET materialize_statistics_on_insert = 1;
SET use_statistics_for_part_pruning = 1;

DROP TABLE IF EXISTS t_mutation_transitive_materialized_update;
DROP TABLE IF EXISTS t_mutation_mixed_transitive_materialized_update;
DROP TABLE IF EXISTS t_mutation_transitive_materialized_key;
DROP TABLE IF EXISTS t_mutation_transitive_materialized_index;
DROP TABLE IF EXISTS t_mutation_clear_transitive_materialized_update;
DROP TABLE IF EXISTS t_mutation_transitive_materialized_statistics;
DROP TABLE IF EXISTS t_mutation_clear_transitive_materialized_key;
DROP TABLE IF EXISTS t_mutation_clear_nested_materialized_update;

CREATE TABLE t_mutation_transitive_materialized_update
(
    id UInt64,
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO t_mutation_transitive_materialized_update (id, c1) VALUES (1, 10);

SELECT id, c1, c2, c3 FROM t_mutation_transitive_materialized_update;

ALTER TABLE t_mutation_transitive_materialized_update UPDATE c1 = 100 WHERE id = 1;

SELECT id, c1, c2, c3 FROM t_mutation_transitive_materialized_update;

DROP TABLE t_mutation_transitive_materialized_update;

CREATE TABLE t_mutation_mixed_transitive_materialized_update
(
    id UInt64,
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c1 + c2
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO t_mutation_mixed_transitive_materialized_update (id, c1) VALUES (1, 10);

SELECT id, c1, c2, c3 FROM t_mutation_mixed_transitive_materialized_update;

ALTER TABLE t_mutation_mixed_transitive_materialized_update UPDATE c1 = 100 WHERE id = 1;

SELECT id, c1, c2, c3 FROM t_mutation_mixed_transitive_materialized_update;

DROP TABLE t_mutation_mixed_transitive_materialized_update;

CREATE TABLE t_mutation_transitive_materialized_index
(
    id UInt64,
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3,
    INDEX idx_c3 c3 TYPE minmax GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY id
SETTINGS min_bytes_for_wide_part = 0;

INSERT INTO t_mutation_transitive_materialized_index (id, c1) VALUES (1, 10), (2, 20);

ALTER TABLE t_mutation_transitive_materialized_index UPDATE c1 = 100 WHERE id = 1;

SELECT id, c1, c2, c3 FROM t_mutation_transitive_materialized_index WHERE c3 = 600 ORDER BY id SETTINGS force_data_skipping_indices = 'idx_c3';

DROP TABLE t_mutation_transitive_materialized_index;

CREATE TABLE t_mutation_transitive_materialized_statistics
(
    id UInt64,
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3 STATISTICS(minmax)
)
ENGINE = MergeTree
ORDER BY id
SETTINGS auto_statistics_types = '';

INSERT INTO t_mutation_transitive_materialized_statistics (id, c1) VALUES (1, 10);
INSERT INTO t_mutation_transitive_materialized_statistics (id, c1) VALUES (2, 20);

ALTER TABLE t_mutation_transitive_materialized_statistics UPDATE c1 = 101 WHERE id = 1;

SELECT count() FROM (EXPLAIN indexes = 1 SELECT id, c1, c2, c3 FROM t_mutation_transitive_materialized_statistics WHERE c3 = 606) WHERE explain LIKE '%Statistics%';
SELECT id, c1, c2, c3 FROM t_mutation_transitive_materialized_statistics WHERE c3 = 606;

DROP TABLE t_mutation_transitive_materialized_statistics;

CREATE TABLE t_mutation_clear_transitive_materialized_update
(
    id UInt64,
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO t_mutation_clear_transitive_materialized_update (id, c1) VALUES (1, 10);

ALTER TABLE t_mutation_clear_transitive_materialized_update CLEAR COLUMN c1;

CREATE TABLE t_mutation_clear_nested_materialized_update
(
    id UInt64,
    n Nested(x UInt64, y UInt64),
    m UInt64 MATERIALIZED arraySum(n.x)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO t_mutation_clear_nested_materialized_update (id, n.x, n.y) VALUES (1, [10, 20], [1, 2]);

ALTER TABLE t_mutation_clear_nested_materialized_update CLEAR COLUMN n;

SELECT id, n.x, m FROM t_mutation_clear_nested_materialized_update;

DROP TABLE t_mutation_clear_nested_materialized_update;

SELECT id, c1, c2, c3 FROM t_mutation_clear_transitive_materialized_update;

DROP TABLE t_mutation_clear_transitive_materialized_update;

CREATE TABLE t_mutation_transitive_materialized_key
(
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3
)
ENGINE = MergeTree
ORDER BY c3;

INSERT INTO t_mutation_transitive_materialized_key (c1) VALUES (10);

ALTER TABLE t_mutation_transitive_materialized_key UPDATE c1 = 100 WHERE c1 = 10; -- { serverError CANNOT_UPDATE_COLUMN }

DROP TABLE t_mutation_transitive_materialized_key;

CREATE TABLE t_mutation_clear_transitive_materialized_key
(
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3
)
ENGINE = MergeTree
ORDER BY c3;

INSERT INTO t_mutation_clear_transitive_materialized_key (c1) VALUES (10);

ALTER TABLE t_mutation_clear_transitive_materialized_key CLEAR COLUMN c1; -- { serverError CANNOT_UPDATE_COLUMN }

DROP TABLE t_mutation_clear_transitive_materialized_key;
