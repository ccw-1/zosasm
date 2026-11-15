*        SUM OF TWO WORDS (HLASM, FIXED COLUMNS)
SUMTAB   CSECT                          THE ONLY SECTION
R1       EQU   1
R2       EQU   2
R3       EQU   3
R13      EQU   13
R14      EQU   14
R15      EQU   15
         STM   R14,R3,12(R13)
         lr    r2,r15                   lower case is folded
         using sumtab,r2
         LA    R3,TABLE
         L     R1,0(,R3)
         A     R1,4(,R3)
         ST    R1,TOTAL
         MVC   TEXT(L'TEXT),                                           X
               =C'done'
         LM    R14,R3,12(R13)
         BR    R14                      RETURN
TABLE    DC    F'10',F'20'
TOTAL    DS    F
TEXT     DC    C'a string that is split at the end of the card,        X
               and goes on here'
         LTORG
         END   SUMTAB
