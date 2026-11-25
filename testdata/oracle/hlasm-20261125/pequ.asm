PEQU     CSECT
         USING PEQU,15
SRC      DS    CL8
ALIAS    EQU   SRC
DST      DS    CL8
         MVC   ALIAS,DST
I1       LR    1,2
J1       EQU   I1
         DC    A(L'ALIAS)
         DC    A(L'J1)
         END
