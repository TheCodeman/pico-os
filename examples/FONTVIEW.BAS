10 REM FONTVIEW.BAS: every font sheet in a folder, one at a time, with
20 REM the size FONT LOAD found. Right, Space or Enter: next; Left or
30 REM Backspace: back; Esc: quit. The folder: /BFONTS, else /NET/bfonts.
40 F$ = "/BFONTS": IF DIR$(F$ + "/*.*") = "" THEN F$ = "/NET/bfonts"
50 SCREEN 12: DIM N$(255): C = 0: K = 1
60 N$ = DIR$(F$ + "/*.*", K): K = K + 1
70 IF N$ = "" THEN 110
80 E$ = UCASE$(RIGHT$(N$, 4))
90 IF (E$ = ".PNG" OR E$ = ".BMP" OR E$ = ".JPG") AND C < 255 THEN C = C + 1: N$(C) = N$
100 GOTO 60
110 IF C = 0 THEN SCREEN 0: PRINT "No font sheets in "; F$: END
120 I = 1
130 CLS: FONT 0
140 GPRINT 8, 460, "Right/Space: next   Left: back   Esc: quit", 1, 7
150 ON ERROR GOTO 400
160 FONT LOAD 1, F$ + "/" + N$(I)
170 ON ERROR GOTO 0
180 GPRINT 8, 4, N$(I); "   "; FONTW(1); "x"; FONTH(1); "   ("; I; "of"; C; ")", 1, 14
190 FONT 1: H = FONTH(1): X = 8: Y = 30
200 T$ = "": FOR A = 32 TO 126: T$ = T$ + CHR$(A): NEXT: GOSUB 500
210 X = 8: Y = Y + H * 2: T$ = "The quick brown fox 0123456789": GOSUB 500
220 X = 8: Y = Y + H * 2: IF Y + H * 2 < 450 THEN T$ = "HELLO!": SC = 2: GOSUB 500
300 K$ = INKEY$: IF K$ = "" THEN 300
310 IF K$ = CHR$(27) THEN 390
320 IF K$ = CHR$(0) + CHR$(75) OR K$ = CHR$(8) THEN I = I - 1 ELSE I = I + 1
330 IF I < 1 THEN I = C
340 IF I > C THEN I = 1
350 GOTO 130
390 FONT CLEAR: SCREEN 0: END
400 FONT 0: GPRINT 8, 30, "Can't load it (error" + STR$(ERR) + ")", 1, 12: RESUME 300
500 REM Draws T$ from (X, Y), at scale SC (1 if 0), going on to the next line at the edge
510 IF SC = 0 THEN SC = 1
520 FOR J = 1 TO LEN(T$)
530   A$ = MID$(T$, J, 1): G = GWIDTH(A$) * SC
540   IF X + G > 632 THEN X = 8: Y = Y + H * SC + 2
550   GPRINT X, Y, A$, SC, -2: X = X + G
560 NEXT: SC = 1: RETURN
