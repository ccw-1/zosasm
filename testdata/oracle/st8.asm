* DSECT in Q and J constants from a resumed section; ER parent.
Y1       CSECT
         EXTRN XA
         WXTRN XW
Y2       CSECT
         DC    A(XA)
         DC    Q(WK)
Y1       CSECT
         DC    J(WK)
         DC    Q(WK2)
         DC    A(XW,XB)
         EXTRN XB
Y3       CSECT
         DC    A(XB)
Y1       CSECT
WK       DSECT
         DS    F
WK2      DSECT
         DS    CL8
         END
