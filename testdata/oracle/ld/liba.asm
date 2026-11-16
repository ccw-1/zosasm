* Library module for the binder test: entries, a table in a second
* class, V and A constants, a weak reference.
LIBA     CSECT
LIBA     AMODE 31
LIBA     RMODE ANY
         ENTRY ADD2
         ENTRY GETTAB
         WXTRN NOPE
ADD2     DS    0H
         USING ADD2,15
         L     2,0(,1)
         L     3,4(,1)
         L     0,0(,2)
         A     0,0(,3)
         LR    15,0
         BR    14
         DROP  15
GETTAB   DS    0H
         USING GETTAB,15
         L     2,=A(TABLE)
         LR    15,2
         BR    14
         LTORG
         DROP  15
DATAC    CATTR RMODE(ANY)
TABLE    DC    F'11,22,33'
TBLEND   DC    A(TBLEND-TABLE)
SELFA    DC    A(LIBA)
SELFV    DC    V(CHK)
WEAK     DC    A(NOPE)
         END
