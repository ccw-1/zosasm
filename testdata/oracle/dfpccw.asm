FEAT     CSECT
         PRINT DATA
D1       DC    ED'1.5'
D2       DC    DD'0.1'
D3       DC    LD'1'
         CNOP  0,8
C1       CCW   X'02',BUF,X'40',80
C2       CCW0  X'06',BUF,X'40',80
C3       CCW1  X'0C',BUF,X'10',80
BUF      DS    CL80
U1       DC    CU'AB'
L1       DC    BL.3'101'
L2       DC    XL.12'ABC'
         END
