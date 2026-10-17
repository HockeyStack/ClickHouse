SET enable_analyzer = 1;
SET enable_materialized_cte = 1;

DROP TABLE IF EXISTS t_mcte_subcolumns;
CREATE TABLE t_mcte_subcolumns (id UInt32, json JSON, d Dynamic, tup Tuple(a UInt32, b String), m Map(String, UInt32), n Nullable(UInt32), arr Array(UInt32))
ENGINE = MergeTree ORDER BY id;
INSERT INTO t_mcte_subcolumns VALUES (1, '{"a": 1}', 1::UInt64, (1, 'one'), map('k', 1), NULL, [1]), (2, '{"a": 2}', 'x', (2, 'two'), map('k', 2), 2, [1, 2]);

SELECT 'single reference';
WITH c AS MATERIALIZED (SELECT * FROM t_mcte_subcolumns)
SELECT json.a, d.UInt64, tup.b, m.keys, n.null, arr.size0 FROM c ORDER BY id;

SELECT 'reused';
WITH c AS MATERIALIZED (SELECT * FROM t_mcte_subcolumns)
SELECT l.json.a, r.d.String, r.tup.a, l.m.values, r.n.null, l.arr.size0 FROM c AS l INNER JOIN c AS r ON l.id = r.id ORDER BY l.id;

SELECT 'reused, only subcolumns are read';
WITH c AS MATERIALIZED (SELECT id, json, d, tup, throwIf(id = 2, 'unused column computed') AS g FROM t_mcte_subcolumns)
SELECT l.json.a, r.d.UInt64, r.tup.b FROM c AS l INNER JOIN c AS r ON l.id = r.id ORDER BY l.id;

DROP TABLE t_mcte_subcolumns;
