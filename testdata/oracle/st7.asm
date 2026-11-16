* Where do pseudo registers (DXD) go with several sections.
X1       CSECT
         DC    Q(DA)
DA       DXD   F
X2       CSECT
         DC    Q(DB,DA)
DB       DXD   D
X3       CSECT
         DC    A(0)
         END
