10 REM SLIDES: a slide show of the pictures in /EXAMPLES/IMAGES
20 REM (BMP, PNG and JPEG). Any key: next picture; Esc: stop.
30 SCREEN 12
40 FOR I = 1 TO 4
50   READ F$, T$
60   CLS: IMAGE "/EXAMPLES/IMAGES/" + F$
70   LOCATE 30, 2: PRINT T$; "  - any key: next, Esc: stop";
80   T = TIMER
90   K$ = INKEY$: IF K$ = "" AND TIMER < T + 6 THEN 90
100  IF K$ = CHR$(27) THEN 200
110 NEXT I
120 CLS
130 FOR Y = 20 TO 400 STEP 48
140   FOR X = 20 TO 600 STEP 48: IMAGE "/EXAMPLES/IMAGES/SMILEY.PNG", X, Y: NEXT X
150 NEXT Y
160 IMAGE "/EXAMPLES/IMAGES/MANDEL.PNG", 400, 300, 200, 150
170 LOCATE 30, 2: PRINT "A sprite at its own size, and a picture in a box. Any key.";
180 WHILE INKEY$ = "": WEND
190 SAVEIMAGE "SLIDES.BMP"
200 SCREEN 0: PRINT "The last screen was saved as SLIDES.BMP (try: view SLIDES.BMP)"
210 DATA MANDEL.PNG, "Mandelbrot set (PNG)", SUNSET.JPG, "Sunset (JPEG, 800x600)"
220 DATA PLASMA.JPG, "Plasma (JPEG)", TESTCARD.BMP, "Test card (24-bit BMP)"
