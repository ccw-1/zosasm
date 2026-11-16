* DSECT in a Q constant: which section gets the pseudo register.
Z1       CSECT
         DC    Q(WD)
Z2       CSECT
WD       DSECT
         DS    F
Z3       CSECT
         DC    A(0)
         END
