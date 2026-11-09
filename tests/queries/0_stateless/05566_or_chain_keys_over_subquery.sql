-- `LogicalExpressionOptimizerPass` groups comparisons by their non-constant operand. The operands below read columns of subqueries.

SET enable_analyzer = 1;
SET optimize_min_equality_disjunction_chain_length = 3;
SET optimize_min_inequality_conjunction_chain_length = 3;
SET optimize_redundant_comparisons = 1;

-- The projection and the GROUP BY key are separate copies of `r`. Both must become the same `in`.
EXPLAIN SYNTAX run_query_tree_passes = 1
SELECT (lower(s) = 'a' OR lower(s) = 'b' OR lower(s) = 'c') AS r, count(), any(j)
FROM (SELECT toString(number) AS s, concat(s, 'x') AS j FROM numbers(10))
GROUP BY r
FORMAT TSVRaw;

SELECT (s = '1' OR s = '2' OR s = '3') AS r, count(), any(j) LIKE '%x'
FROM (SELECT toString(number) AS s, concat(s, 'x') AS j FROM numbers(10))
GROUP BY r
ORDER BY r;

-- A chain of CTEs: `notEquals` becomes `notIn` and `equals` becomes `in`.
EXPLAIN SYNTAX run_query_tree_passes = 1
WITH
    t0 AS (SELECT toString(number) AS s FROM numbers(10)),
    t1 AS (SELECT *, concat(s, 'x') AS l FROM t0)
SELECT count()
FROM t1
WHERE l != '1x' AND l != '2x' AND l != '3x' AND (s = '1' OR s = '4' OR s = '5')
FORMAT TSVRaw;

WITH
    t0 AS (SELECT toString(number) AS s FROM numbers(10)),
    t1 AS (SELECT *, concat(s, 'x') AS l FROM t0)
SELECT count()
FROM t1
WHERE l != '1x' AND l != '2x' AND l != '3x' AND (s = '1' OR s = '4' OR s = '5');

-- Contradicting comparisons on one subquery column fold to false.
EXPLAIN SYNTAX run_query_tree_passes = 1
SELECT count() FROM (SELECT toString(number) AS s FROM numbers(10)) WHERE s = '1' AND s = '2'
FORMAT TSVRaw;

-- Columns of two different table expressions are separate operands.
EXPLAIN SYNTAX run_query_tree_passes = 1
SELECT count()
FROM (SELECT toString(number) AS s FROM numbers(10)) AS a, (SELECT toString(number) AS s FROM numbers(10)) AS b
WHERE a.s = '1' OR a.s = '2' OR b.s = '3' OR a.s = '4'
FORMAT TSVRaw;

SELECT count()
FROM (SELECT toString(number) AS s FROM numbers(10)) AS a, (SELECT toString(number) AS s FROM numbers(10)) AS b
WHERE a.s = '1' OR a.s = '2' OR b.s = '3' OR a.s = '4';
