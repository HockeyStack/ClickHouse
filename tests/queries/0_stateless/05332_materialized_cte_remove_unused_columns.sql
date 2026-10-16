SET enable_analyzer = 1;
SET enable_materialized_cte = 1;

SELECT 'unused column is not computed';
WITH t AS MATERIALIZED (SELECT number AS a, throwIf(number = 5, 'b computed') AS b FROM numbers(10))
SELECT sum(a) FROM (SELECT a FROM t UNION ALL SELECT a FROM t);

SELECT 'storage keeps the columns of all references';
WITH t AS MATERIALIZED (SELECT number AS a, number * 10 AS b, throwIf(number = 5, 'c computed') AS c FROM numbers(10))
SELECT sum(x) FROM (SELECT a AS x FROM t UNION ALL SELECT b AS x FROM t);

WITH t AS MATERIALIZED (SELECT number AS a, number * 10 AS b, throwIf(number = 5, 'c computed') AS c FROM numbers(10))
SELECT sum(l.a), sum(r.b) FROM t AS l INNER JOIN t AS r ON l.a = r.a;

SELECT 'no column is read';
WITH t AS MATERIALIZED (SELECT number AS a, throwIf(number = 5, 'b computed') AS b FROM numbers(10))
SELECT count() FROM t AS l, t AS r;

SELECT 'nested materialized CTE';
WITH
    b AS MATERIALIZED (SELECT number AS x, throwIf(number = 5, 'b.y computed') AS y FROM numbers(10)),
    a AS MATERIALIZED (SELECT x AS p, y AS q FROM b)
SELECT sum(p) FROM (SELECT p FROM a UNION ALL SELECT p FROM a UNION ALL SELECT x FROM b);

SELECT 'subquery inside the CTE body';
WITH t AS MATERIALIZED (SELECT a, b FROM (SELECT number AS a, throwIf(number = 5, 'b computed') AS b FROM numbers(10)))
SELECT sum(a) FROM (SELECT a FROM t UNION ALL SELECT a FROM t);

SELECT 'scalar subquery reads another column';
WITH t AS MATERIALIZED (SELECT number AS a, number * 10 AS b FROM numbers(10))
SELECT (SELECT max(b) FROM t), sum(a) FROM (SELECT a FROM t UNION ALL SELECT a FROM t);

SELECT 'DISTINCT keeps all columns';
WITH t AS MATERIALIZED (SELECT DISTINCT number % 2 AS a, number % 3 AS b FROM numbers(6))
SELECT count() FROM (SELECT a FROM t UNION ALL SELECT a FROM t);

WITH t AS MATERIALIZED (SELECT number % 2 AS a, number % 3 AS b FROM numbers(6) UNION DISTINCT SELECT 0, 0)
SELECT count() FROM (SELECT a FROM t UNION ALL SELECT a FROM t);

SELECT 'aggregate without GROUP BY';
WITH t AS MATERIALIZED (SELECT sum(number) AS s, count() AS c FROM numbers(10) WHERE 0)
SELECT count() FROM (SELECT 1 FROM t UNION ALL SELECT 1 FROM t);

SELECT 'GLOBAL IN on a remote table';
WITH t AS MATERIALIZED (SELECT number AS a, number * 10 AS b, throwIf(number = 5, 'c computed') AS c FROM numbers(10))
SELECT count() FROM remote('127.0.0.{1,2}', numbers(10))
WHERE number GLOBAL IN (SELECT a FROM t) AND number * 10 GLOBAL IN (SELECT b FROM t);

SELECT 'IN reads all columns of the table';
WITH t AS MATERIALIZED (SELECT number AS a, number * 2 AS b FROM numbers(5))
SELECT count() FROM t WHERE (a, a * 2) IN t;

SELECT 'scalar subquery with two references';
WITH t AS MATERIALIZED (SELECT number AS a, number * 10 AS b FROM numbers(10))
SELECT (SELECT sum(x.a) FROM t AS x, t AS y), sum(l.b) FROM t AS l, t AS r;
