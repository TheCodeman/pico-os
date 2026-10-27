10 REM I2CSCAN.BAS: lists the I2C devices that answer, then reads a few
20 REM bytes from the first one. Wire SDA to GP2 and SCL to GP3 (I2C1),
30 REM with 3.3 V and GND; most breakout boards have their own pull-ups.
40 I2C OPEN 2, 3, 100
50 PRINT "Devices: ";: I2C SCAN
60 FOR A = 8 TO 119
70   ON ERROR GOTO 200
80   B = I2CREAD(A)
90   ON ERROR GOTO 0
100  PRINT "First at &H"; HEX$(A); ": registers 0-7 read";
110  R$ = I2CREAD$(A, 8, 0)
120  FOR I = 1 TO 8: PRINT " "; HEX$(ASC(MID$(R$, I, 1)));: NEXT: PRINT
130  GOTO 300
200 RESUME 210
210 NEXT A
220 PRINT "No device to read."
300 ON ERROR GOTO 0: I2C CLOSE
