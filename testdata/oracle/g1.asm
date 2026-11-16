MAIN     CSECT
MAIN     AMODE 31
MAIN     RMODE ANY
         ENTRY ALTENT
         EXTRN EXTSYM
         WXTRN WEAKSYM
         USING MAIN,12
         STM   14,12,12(13)
         LR    12,15
         LA    2,DATA
         L     3,ADDRS
         L     4,ADDRS+4
         L     15,=V(EXTSYM)
         BASR  14,15
ALTENT   DS    0H
         BRAS  5,NEXT
NEXT     DS    0H
         LM    14,12,12(13)
         BR    14
DATA     DC    C'THIS TEXT IS LONGER THAN FIFTY-SIX BYTES, SO'
         DC    C' IT CONTINUES IN A SECOND RECORD OR MORE'
ADDRS    DC    A(DATA),A(EXTSYM),V(EXTSYM)
         DC    A(WEAKSYM)
         DC    AL2(DATA-MAIN),Y(NEXT-MAIN)
LONGNAME_WITH_MORE_THAN_EIGHT_CHARACTERS DS 0H
         DC    F'1',H'2',X'0A0B',P'123',Z'45'
         LTORG
         END   MAIN
