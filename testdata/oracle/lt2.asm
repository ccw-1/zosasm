LT2      CSECT
         USING LT2,15
         L     1,=XL4'1'
         DC    X'00'
         LTORG
         DC    X'11'
         L     1,=AL1(1,2)
         DC    X'22'
         LTORG
         DC    X'33'
         L     1,=XL3'1'
         L     1,=2H'1'
         L     1,=CL5'ABCDE'
         L     1,=X'0102030405060708'
         L     1,=C'AB'
         L     1,=CL16'A'
         L     1,=B'101'
         L     1,=P'123'
         L     1,=Z'12'
         L     1,=3C'A'
         DC    X'44'
         L     1,=S(LT2)
         L     1,=A(*)
         L     1,=V(EXT)
         L     1,=Y(LT2)
         L     1,=FD'1'
         L     1,=E'1'
         L     1,=EB'1'
         END
