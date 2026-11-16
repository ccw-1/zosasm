* LOCTR piece starts and alignment.
Q1       CSECT
         DC    X'01'
L1       LOCTR
         DC    X'02'
A1       DC    F'3'
         DC    X'04'
A2       DC    D'5'
         DC    X'09'
L2       LOCTR
         DC    X'06'
A3       DC    H'7'
         DC    X'0A'
L3       LOCTR
         DC    X'0B'
A4       DC    X'0C'
         DC    A(A1,A2,A3,A4)
         END
