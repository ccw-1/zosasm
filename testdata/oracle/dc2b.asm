DC2      CSECT
         DC    Y(L1)
         DC    AL2(L1)
         DC    AL3(L1)
         DC    Y(X'10',L1-DC2)
         DC    VL3(EXT1)
         DC    AL2(EXT1)
         DC    AL3(EXT1)
         DC    AL2(L1-DC2)
         DC    AD(EXT1+4)
         DC    A(EXT1-L1)
         DC    A(L1-EXT1)
         DC    A(EXT1-EXT2)
L1       DC    A(0)
         EXTRN EXT1,EXT2
         END
