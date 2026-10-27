10 REM BLINK: flash the board's LED (GP25). Ctrl-C stops.
20 PRINT "Blinking the LED. Ctrl-C stops."
30 FOR I = 1 TO 30
40   PIN LED, 1: PAUSE 200
50   PIN LED, 0: PAUSE 300
60 NEXT I
70 PRINT "Done"
