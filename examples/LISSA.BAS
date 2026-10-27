10 REM Lissajous figures in 16 colours
20 SCREEN 12
30 FOR K = 1 TO 6
40   COLOR K + 8
50   PSET (320, 240 + 200 * SIN(K))
60   FOR T = 0 TO 6.3 STEP .01
70     LINE -(320 + 200 * SIN(K * T), 240 + 200 * SIN((K + 1) * T + K))
80   NEXT T
90 NEXT K
100 LOCATE 29, 1: PRINT "Press a key";
110 WHILE INKEY$ = "": WEND
120 SCREEN 0
