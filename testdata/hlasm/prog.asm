*        SAVE AREA DEMO, FIXED COLUMNS WITH CONTINUATION
PROG     CSECT                          THE ENTRY SECTION
PROG     AMODE 31
PROG     RMODE ANY
R0       EQU   0
R1       EQU   1
R2       EQU   2
R12      EQU   12
R13      EQU   13
R14      EQU   14
R15      EQU   15
BASE     EQU   R12                      ALIAS
FP0      EQU   0                        FLOATING POINT REGISTER 0
         STM   R14,R12,12(R13)          SAVE CALLERS REGISTERS
         LR    BASE,R15
         USING PROG,BASE
         la    r1,work                  lower case source
         LD    FP0,=D'1.5'
         MVC   MSG(L'MSG),=C'Hello, World'
         MVC   LONGNAME(8),                                            X
               SOURCE
         DC    C'A STRING THAT RUNS TO THE END OF THE CARD,            X
               ON THE NEXT ONE'
         DC    X'f0f1',A(PROG),AL2(LEN)       LOWER CASE HEX
LEN      EQU   *-PROG
         LM    R14,R12,12(R13)
         BR    R14                      RETURN
         LTORG
WORK     DS    18F
MSG      DC    CL20' '
LONGNAME DS    CL8
SOURCE   DC    CL8'ABCDEFGH'
         END   PROG
