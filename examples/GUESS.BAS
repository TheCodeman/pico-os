10 REM Guess the number
20 RANDOMIZE TIMER
30 CLS: COLOR 14: PRINT "I'm thinking of a number from 1 to 100.": COLOR 7
40 N = INT(RND * 100) + 1: T = 0
50 INPUT "Your guess"; G
60 T = T + 1
70 IF G < N THEN PRINT "Higher...": GOTO 50
80 IF G > N THEN PRINT "Lower...": GOTO 50
90 COLOR 10: PRINT "Got it in"; T; "tries!": COLOR 7
100 BEEP
110 INPUT "Play again (Y/N)"; A$
120 IF LEFT$(UCASE$(A$), 1) = "Y" THEN 30
