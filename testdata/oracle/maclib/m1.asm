         MACRO
         M1    &A
         AIF   ('&A' EQ 'Y').GO
         MNOTE 4, 'TRAILING COMMA BLANK'
.GO      ANOP
         MEND
         MACRO
         M2
         LCLB  @QQ
&QQ      SETB  1
         AIF   (NOT(&QQ)).X
         MNOTE 0,'QQ IS SET'
.X       ANOP
         MEND
         MACRO
         M3                             !                          @T4C
         MNOTE 0,'M3 BODY'
         MEND
         MACRO
         M4
         AGO   .Z
Macro Reference". Do one of the following: #
.Z       MNOTE 0,'M4 BODY'
         MEND
T        CSECT
         M1    N
         M2
         M3
         M4
         END
