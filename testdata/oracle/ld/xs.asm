* Two CSECTs in one object: relative references between sections.
XA       CSECT
XA       AMODE 31
XA       RMODE ANY
         LA    15,7
         BR    14
XA2      DS    0H
         LA    15,9
         BR    14
XB       CSECT
XB       AMODE 31
XB       RMODE ANY
         STM   14,12,12(13)
         LR    12,15
         USING XB,12
         SR    6,6
         BRASL 14,XA
         C     15,=F'7'
         BNE   N1
         LA    6,1(,6)
N1       DS    0H
         LARL  3,XA
         C     3,AXA
         BNE   N2
         LA    6,2(,6)
N2       DS    0H
         LARL  3,XB
         C     3,AXB
         BNE   N3
         LA    6,4(,6)
N3       DS    0H
         LARL  3,XA2
         C     3,AXA2
         BNE   N4
         LA    6,8(,6)
N4       DS    0H
         BRASL 14,XA2
         C     15,=F'9'
         BNE   N5
         LA    6,16(,6)
N5       DS    0H
         LR    15,6
         L     14,12(,13)
         LM    0,12,20(13)
         BR    14
AXA      DC    A(XA)
AXB      DC    A(XB)
AXA2     DC    A(XA2)
         LTORG
         END
