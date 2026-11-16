* LOCTR piece growth after a later piece was started.
Q2       CSECT
         DC    X'01'
L1       LOCTR
         DC    X'02'
A1       DC    F'3'
         DC    X'04'
Q2       LOCTR
         DC    X'05'
A2       DC    D'6'
         DC    A(A1,A2)
         END
