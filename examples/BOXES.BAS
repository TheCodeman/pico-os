10 REM Colour boxes drawn with the PC line-drawing characters
20 CLS
30 DEF FNR$(C, N) = STRING$(N, C)
40 FOR B = 0 TO 5
50   X = 3 + B * 12: Y = 3 + B * 3: W = 20: H = 6
60   COLOR B + 9, B + 1
70   LOCATE Y, X: PRINT CHR$(201); FNR$(205, W - 2); CHR$(187);
80   FOR R = 1 TO H - 2
90     LOCATE Y + R, X: PRINT CHR$(186); SPACE$(W - 2); CHR$(186);
100  NEXT R
110  LOCATE Y + H - 1, X: PRINT CHR$(200); FNR$(205, W - 2); CHR$(188);
120  LOCATE Y + 2, X + 3: PRINT "Box"; B + 1;
130 NEXT B
140 COLOR 7, 0
150 LOCATE 26, 1: PRINT CHR$(176); CHR$(177); CHR$(178); CHR$(219); " shades and blocks"
160 SOUND 523, 3: SOUND 659, 3: SOUND 784, 6
