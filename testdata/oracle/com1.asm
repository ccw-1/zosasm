* COM sections.
C1       CSECT
         DC    A(CM1,CM2)
         LARL  3,CM1
CM1      COM
         DS    CL16
CM2      COM
         DS    F
         END
