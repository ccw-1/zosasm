PW       CSECT
         USING PW,15
DATA     DC    CL8'ABCDEFGH'
         CCW   1,DATA,X'20',100
         CCW0  2,DATA,X'40',200
         CCW1  3,DATA,X'60',300
         DC    S(0(1))
         DC    S(DATA)
         DC    SY(8(2))
         DC    SY(DATA)
         END
