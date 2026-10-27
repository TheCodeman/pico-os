10 REM FADE: fade the LED up and down with PWM. Ctrl-C stops.
20 PRINT "Fading the LED. Ctrl-C stops."
30 FOR R = 1 TO 5
40   FOR D = 0 TO 100 STEP 2: PWM LED, 1000, D: PAUSE 10: NEXT D
50   FOR D = 100 TO 0 STEP -2: PWM LED, 1000, D: PAUSE 10: NEXT D
60 NEXT R
70 PWM LED, 0
