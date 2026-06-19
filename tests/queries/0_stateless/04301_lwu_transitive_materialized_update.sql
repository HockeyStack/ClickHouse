SET enable_lightweight_update = 1;

DROP TABLE IF EXISTS t_lwu_transitive_materialized_update;
DROP TABLE IF EXISTS t_lwu_mixed_transitive_materialized_update;
DROP TABLE IF EXISTS t_lwu_transitive_materialized_key;
DROP TABLE IF EXISTS t_lwu_transitive_materialized_index;

CREATE TABLE t_lwu_transitive_materialized_update
(
    id UInt64,
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3
)
ENGINE = MergeTree
ORDER BY id
SETTINGS enable_block_number_column = 1,
         enable_block_offset_column = 1;

INSERT INTO t_lwu_transitive_materialized_update (id, c1) VALUES (1, 10);

SELECT id, c1, c2, c3 FROM t_lwu_transitive_materialized_update;

UPDATE t_lwu_transitive_materialized_update SET c1 = 100 WHERE id = 1;

SELECT id, c1, c2, c3 FROM t_lwu_transitive_materialized_update;

DROP TABLE t_lwu_transitive_materialized_update;

CREATE TABLE t_lwu_mixed_transitive_materialized_update
(
    id UInt64,
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c1 + c2
)
ENGINE = MergeTree
ORDER BY id
SETTINGS enable_block_number_column = 1,
         enable_block_offset_column = 1;

INSERT INTO t_lwu_mixed_transitive_materialized_update (id, c1) VALUES (1, 10);

SELECT id, c1, c2, c3 FROM t_lwu_mixed_transitive_materialized_update;

UPDATE t_lwu_mixed_transitive_materialized_update SET c1 = 100 WHERE id = 1;

SELECT id, c1, c2, c3 FROM t_lwu_mixed_transitive_materialized_update;

DROP TABLE t_lwu_mixed_transitive_materialized_update;

CREATE TABLE t_lwu_transitive_materialized_index
(
    id UInt64,
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3,
    INDEX idx_c3 c3 TYPE minmax GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY id
SETTINGS enable_block_number_column = 1,
         enable_block_offset_column = 1,
         min_bytes_for_wide_part = 0;

INSERT INTO t_lwu_transitive_materialized_index (id, c1) VALUES (1, 10), (2, 20);

UPDATE t_lwu_transitive_materialized_index SET c1 = 100 WHERE id = 1;

SELECT id, c1, c2, c3 FROM t_lwu_transitive_materialized_index WHERE c3 = 600 ORDER BY id SETTINGS force_data_skipping_indices = 'idx_c3';

DROP TABLE t_lwu_transitive_materialized_index;

CREATE TABLE t_lwu_transitive_materialized_key
(
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3
)
ENGINE = MergeTree
ORDER BY c3
SETTINGS enable_block_number_column = 1,
         enable_block_offset_column = 1;

INSERT INTO t_lwu_transitive_materialized_key (c1) VALUES (10);

UPDATE t_lwu_transitive_materialized_key SET c1 = 100 WHERE c1 = 10; -- { serverError CANNOT_UPDATE_COLUMN }

DROP TABLE t_lwu_transitive_materialized_key;
