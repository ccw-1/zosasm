PLC      CSECT
L1       LOCTR
X        DC    F'1'
L2       LOCTR
Y        DS    0D
Z        DC    F'2'
L3       LOCTR
W        DC    F'3'
L4       LOCTR
V        DC    A(X,Z,W)
         END
