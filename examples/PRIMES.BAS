10 REM Sieve of Eratosthenes: the primes below 1000
20 N = 1000
30 DIM S(N)
40 FOR I = 2 TO SQR(N)
50   IF S(I) THEN 80
60   FOR J = I * I TO N STEP I: S(J) = 1: NEXT J
80 NEXT I
90 C = 0
100 FOR I = 2 TO N
110   IF S(I) = 0 THEN PRINT RIGHT$("     " + STR$(I), 5);: C = C + 1
120 NEXT I
130 PRINT
140 PRINT C; "primes below"; N
