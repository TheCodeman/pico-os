10 REM MUSIC.BAS: a MOD song plays in the background while this draws.
20 REM The ball changes colour with the song's rows. Any key ends it.
30 SCREEN 1: CLS
40 ON ERROR GOTO 170: MUSIC "/EXAMPLES/DEMO.MOD"
45 ON ERROR GOTO 0
50 X = 160: Y = 100: DX = 3: DY = 2
60 WHILE INKEY$ = ""
70   LINE (X - 5, Y - 5)-(X + 5, Y + 5), 0, BF
80   X = X + DX: Y = Y + DY
90   IF X < 8 OR X > 311 THEN DX = -DX
100  IF Y < 22 OR Y > 193 THEN DY = -DY
110  LINE (X - 5, Y - 5)-(X + 5, Y + 5), 1 + (MUSIC(2) \ 4) MOD 3, BF
120  LOCATE 1, 1: PRINT "Position"; MUSIC(1); " row"; MUSIC(2); "  "
130 WEND
140 MUSIC STOP
150 SCREEN 0
160 END
170 SCREEN 0: PRINT "No /EXAMPLES/DEMO.MOD: make it with tools/make_demo_mod.py,"
180 PRINT "or put a MOD or XM song of your own in line 40.": END
