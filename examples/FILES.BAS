10 REM FILES: sequential and random-access files
20 PRINT "Writing NOTES.TXT..."
30 OPEN "NOTES.TXT" FOR OUTPUT AS #1
40 PRINT #1, "Shopping list"
50 WRITE #1, "Apples", 6, "Milk, semi-skimmed", 2
60 CLOSE #1
70 OPEN "NOTES.TXT" FOR APPEND AS #1
80 WRITE #1, "Bread", 1, "Tea", 80
90 CLOSE #1
100 PRINT "Reading it back:"
110 OPEN "NOTES.TXT" FOR INPUT AS #1
120 LINE INPUT #1, T$: PRINT "Title: "; T$
130 WHILE NOT EOF(1)
140   INPUT #1, ITEM$, N
150   PRINT "  "; ITEM$; TAB(26); N
160 WEND
170 CLOSE #1
180 PRINT
190 PRINT "Random access: PEOPLE.DAT, 22-byte records"
200 OPEN "PEOPLE.DAT" FOR RANDOM AS #2 LEN = 22
210 FIELD #2, 20 AS NM$, 2 AS AGE$
220 FOR I = 1 TO 3
230   READ A$, B
240   LSET NM$ = A$: LSET AGE$ = MKI$(B)
250   PUT #2, I
260 NEXT I
270 DATA "Ada", 36, "Grace", 85, "Alan", 41
280 GET #2, 2
290 PRINT "Record 2: ["; NM$; "] age"; CVI(AGE$)
300 PRINT "Records in the file:"; LOF(2) / 22
310 CLOSE #2
320 KILL "PEOPLE.DAT"
330 PRINT "Done. NOTES.TXT is left on the drive: TYPE it from the shell."
