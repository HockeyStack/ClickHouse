-- A `CLEAR COLUMN` on a `Nested` group clears the physical subcolumns, so the
-- `MATERIALIZED` columns derived from them must be recomputed with the default value.

SET mutations_sync = 1;

DROP TABLE IF EXISTS t_clear_column_nested_materialized;
CREATE TABLE t_clear_column_nested_materialized
(
    id UInt64,
    n Nested(x UInt64, y UInt64),
    m UInt64 MATERIALIZED arraySum(n.x)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO t_clear_column_nested_materialized (id, n.x, n.y) VALUES (1, [10, 20], [1, 2]);
SELECT id, n.x, m FROM t_clear_column_nested_materialized;

ALTER TABLE t_clear_column_nested_materialized CLEAR COLUMN n;
SELECT id, n.x, m FROM t_clear_column_nested_materialized;

DROP TABLE t_clear_column_nested_materialized;
