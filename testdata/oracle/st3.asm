* EXTRN, WXTRN, V, Q, J, R constants, DXD, CXD, literals.
T3       CSECT
T3       AMODE 31
T3       RMODE ANY
         EXTRN E1,E2
         WXTRN W1
         USING T3,15
         DC    A(E1,W1)
         DC    V(E2)
         DC    A(E1+8)
         DC    A(E1-E2)
         L     1,=A(E1)
         L     2,=V(E2)
         L     3,=F'5'
         L     4,=A(T3)
         LARL  5,E1
         BRASL 14,E2
         DC    Q(DX1)
         CXD
         LTORG
DX1      DXD   2F
DX2      DXD   CL12
         END
