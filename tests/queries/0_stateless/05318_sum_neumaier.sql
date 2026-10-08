-- Large values that cancel each other: sumNeumaier keeps the small terms.
SELECT sum(x), sumNeumaier(x), sumNeumaierHiLo(x) FROM values('x Float64', 1, 1e100, 1, -1e100);
SELECT sum(x), sumKahan(x), sumNeumaier(x), sumNeumaierHiLo(x) FROM values('x Float64', 1000, 1e-18, -1000);
SELECT sumNeumaier(0.1), sumNeumaierHiLo(0.1) FROM numbers(10);

SELECT toTypeName(sumNeumaier(1)), toTypeName(sumNeumaierHiLo(1)), toTypeName(sumNeumaier(toDecimal32(1, 2)))
SETTINGS print_pretty_type_names = 0;
SELECT sumNeumaier(number), sumNeumaierHiLo(number), sumNeumaier(toFloat32(number)) FROM numbers(1000);
SELECT sumNeumaier(toDecimal32(number, 2)) FROM numbers(10);
SELECT sumNeumaierHiLo(toDecimal32(1, 2)); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }

SELECT sumNeumaier(x), sumNeumaierHiLo(x) FROM values('x Float64', 1) WHERE x > 1;
SELECT sumNeumaier(x), sumNeumaierHiLo(x) FROM values('x Float64', 1, inf, 1);
SELECT sumNeumaier(x), sumNeumaierHiLo(x) FROM values('x Float64', 1, inf, -inf);
SELECT sumNeumaier(x), sumNeumaierHiLo(x) FROM values('x Float64', 1e308, 1e308, 1);

-- Each partial sum, merge, and serialized state must keep the exact error.
-- The sum of the rows 1e20, 1, -1e20, ... is the count of the rows with 1.
SELECT sumNeumaier(x), sumNeumaierHiLo(x)
FROM (SELECT [1e20, 1, -1e20][number % 3 + 1] AS x FROM numbers_mt(3000000))
SETTINGS max_threads = 8, max_block_size = 1000;

SELECT count(), countIf(s = ones), countIf(hi_lo.hi = ones AND hi_lo.lo = 0)
FROM
(
    SELECT number % 10 AS k, sumNeumaier(x) AS s, sumNeumaierHiLo(x) AS hi_lo, countIf(x = 1) AS ones
    FROM (SELECT number, [1e20, 1, -1e20][number % 3 + 1] AS x FROM numbers_mt(3000000))
    GROUP BY k
)
SETTINGS max_threads = 8, max_block_size = 1000, group_by_two_level_threshold = 1;

SELECT sumNeumaierMerge(s), sumNeumaierHiLoMerge(hi_lo)
FROM
(
    SELECT number % 7 AS k, sumNeumaierState(x) AS s, sumNeumaierHiLoState(x) AS hi_lo
    FROM (SELECT number, [1e20, 1, -1e20][number % 3 + 1] AS x FROM numbers(3000))
    GROUP BY k
);

-- NULL and the If combinator skip their rows.
SELECT sumNeumaier(x), sumNeumaierHiLo(x), sumNeumaierIf(x, x != 1), sumNeumaierHiLoIf(x, x != 1)
FROM values('x Nullable(Float64)', 1, NULL, 1e100, 1, -1e100, NULL);
SELECT sumNeumaierHiLo(x) FROM values('x Nullable(Float64)', NULL);

-- A moving frame adds from a row in the middle of a block.
SELECT countIf(a != b), countIf(c != d), countIf(a != e)
FROM
(
    SELECT
        sum(x) OVER w AS a, sumNeumaier(x) OVER w AS b,
        sumIf(x, x > 3) OVER w AS c, sumNeumaierIf(x, x > 3) OVER w AS d,
        tupleElement(sumNeumaierHiLo(x) OVER w, 'hi') AS e
    FROM (SELECT number, if(number % 5 = 0, NULL, number % 7) AS x FROM numbers(1000))
    WINDOW w AS (ORDER BY number ROWS BETWEEN 3 PRECEDING AND CURRENT ROW)
)
SETTINGS max_block_size = 10;

-- The window of a running sum keeps its precision under a large history.
SELECT n, s, (s.hi - first_s.hi) + (s.lo - first_s.lo), p - first_p
FROM
(
    SELECT n, s, p, first_value(s) OVER (ORDER BY n) AS first_s, first_value(p) OVER (ORDER BY n) AS first_p
    FROM
    (
        SELECT n, sumNeumaierHiLo(x) OVER (ORDER BY n) AS s, sum(x) OVER (ORDER BY n) AS p
        FROM values('n UInt8, x Float64', (1, 1e20), (2, 1), (3, 1), (4, 0.5))
    )
)
ORDER BY n;
