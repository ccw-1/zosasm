* ER parent section: the section current at the EXTRN statement.
P1       CSECT
         DC    A(XA,XB,XC)
         EXTRN XU
P2       CSECT
         EXTRN XA
         WXTRN XB
         DC    A(XA,XD)
P3       CSECT
         EXTRN XC,XD
         DC    A(XC)
         END
