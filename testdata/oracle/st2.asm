* LOCTR, ORG, ALIGN, CNOP, several CSECTs interleaved.
T1       CSECT
         DC    F'1'
L1       LOCTR
         DC    F'2'
L1B      DC    F'3'
T1       LOCTR
         DC    F'4'
         ORG   T1+2
         DC    H'5'
         CNOP  0,8
         DC    F'6'
T2       CSECT
         DC    F'7'
T1       CSECT
         DC    A(L1B,T2)
         DC    A(L1B-T1)
         DC    Y(L1B-T1)
         LARL  3,L1B
         LARL  4,T2
         END
