* J, R, QY constants, ER parent section, external DSECT.
S6       CSECT
         EXTRN E1
S6B      CSECT
         EXTRN E2
         WXTRN E3
S6       CSECT
         ENTRY EN1
EN1      DS    0H
         DC    A(E1,E2,E3)
         DC    R(EN1)
         DC    V(E1)
         DC    J(DXA)
         DC    Q(DXA)
         DC    QY(DXA)
DXA      DXD   F
DXB      DXD   CL8
         DC    Q(DXB)
         END
