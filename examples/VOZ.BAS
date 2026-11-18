10 REM VOZ.BAS - SAY habla por la tarjeta de sonido (formantes, port de FTTS)
20 SAY "-v m hola, soy PC BASIC. ahora puedo hablar."
30 FOR I = 1 TO 3
40 SAY "contando " + STR$(I)
50 NEXT I
60 SAY "-v f -r 1.2 y ahora con voz de mujer, un poco mas rapido."
70 SAY "-v r soy un robot"
80 END
