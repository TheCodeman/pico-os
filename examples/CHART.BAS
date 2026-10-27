10 REM The 256 colours of SCREEN 13, in 16 x 16 filled boxes
20 SCREEN 13
30 FOR C = 0 TO 255
40   X = (C MOD 16) * 20: Y = (C \ 16) * 12
50   LINE (X, Y)-(X + 18, Y + 10), C, BF
60 NEXT C
70 LINE (0, 194)-(319, 199), RGB(255, 128, 0), BF
80 WHILE INKEY$ = "": WEND
90 SCREEN 0
