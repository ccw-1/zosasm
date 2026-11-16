SECA     CSECT
SECA     AMODE 24
SECA     RMODE 24
         ENTRY ENT1,ENT2
ENT1     AMODE 31
ENT2     RMODE ANY
         EXTRN OUTSYM
         BR    14
ENT1     DS    0H
         BR    14
ENT2     DS    0H
         BR    14
SECB     CSECT
SECB     AMODE ANY
SECB     RMODE 31
         BRASL 14,SECA
         BRASL 14,OUTSYM
         LARL  3,SECA
         LGRL  4,CELL
CELL     DC    A(SECB,OUTSYM,SECA)
         DC    A(SECA+4)
         DC    AL4(SECB-SECA)
         DC    AD(CELL)
DSA      DSECT
DFLD     DS    F
SECC     CSECT
         USING DSA,5
         L     1,DFLD
         END
