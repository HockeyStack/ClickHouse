-- `CLEAR COLUMN` must refuse a column whose `MATERIALIZED` closure holds a key column,
-- because the recompute would rewrite the sort key.

SET mutations_sync = 1;

DROP TABLE IF EXISTS t_clear_column_materialized_key;
CREATE TABLE t_clear_column_materialized_key
(
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3
)
ENGINE = MergeTree
ORDER BY c3;

INSERT INTO t_clear_column_materialized_key (c1) VALUES (10);
ALTER TABLE t_clear_column_materialized_key CLEAR COLUMN c1; -- { serverError CANNOT_UPDATE_COLUMN }

-- The same clear is accepted when the derived `MATERIALIZED` columns are not part of the key.
DROP TABLE IF EXISTS t_clear_column_materialized_not_key;
CREATE TABLE t_clear_column_materialized_not_key
(
    id UInt64,
    c1 UInt64,
    c2 UInt64 MATERIALIZED c1 * 2,
    c3 UInt64 MATERIALIZED c2 * 3
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO t_clear_column_materialized_not_key (id, c1) VALUES (1, 10);
ALTER TABLE t_clear_column_materialized_not_key CLEAR COLUMN c1;
SELECT id, c1, c2, c3 FROM t_clear_column_materialized_not_key;

DROP TABLE t_clear_column_materialized_not_key;
DROP TABLE t_clear_column_materialized_key;
