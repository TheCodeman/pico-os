10 REM SERIAL: a tiny terminal on COM1 (GP4 = TX, GP5 = RX, 3.3 V levels only)
20 REM What you type is sent; what arrives is shown. Esc quits.
30 OPEN "COM1:9600,N,8,1" AS #1
40 PRINT "Terminal on COM1, 9600 baud 8N1 (TX GP4, RX GP5). Esc quits."
50 K$ = INKEY$
60 IF K$ = CHR$(27) THEN 100
70 IF K$ <> "" THEN PRINT #1, K$;
80 IF LOC(1) > 0 THEN PRINT INPUT$(LOC(1), #1);
90 GOTO 50
100 CLOSE #1: PRINT: PRINT "Closed."
