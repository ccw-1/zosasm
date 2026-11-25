PFW2     CSECT
ALIAS    EQU   SRC
SRC      DS    CL8
DST      DS    CL8
         MVC   ALIAS,DST
X        EQU   Y+4
Y        DC    F'1'
         DC    A(X)
         END
