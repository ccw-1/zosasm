SECA     CSECT
         EXTRN OUTSYM
         BRASL 14,OUTSYM
         BRASL 14,OUTSYM+4
         BRASL 14,OUTSYM-6
         LARL  3,OUTSYM+2
         BRCL  15,OUTSYM
         BRAS  14,*+4
         BR    14
         DC    A(OUTSYM+4)
         DC    A(L2)
L2       DS    0H
SECB     CSECT
         BRASL 14,L2
         BRASL 14,SECA
         LARL  3,SECA+4
         LARL  3,L2
         END
