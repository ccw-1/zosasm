* Section kinds and AMODE/RMODE combinations.
A1       CSECT
A1       AMODE 24
A1       RMODE 24
         DC    F'1'
A2       CSECT
A2       AMODE 31
A2       RMODE 31
         DC    F'2'
A3       CSECT
A3       AMODE ANY
A3       RMODE ANY
         DC    F'3'
A4       CSECT
A4       AMODE 64
A4       RMODE 64
         DC    F'4'
A5       RSECT
A5       AMODE 31
A5       RMODE ANY
         DC    F'5'
A6       CSECT
         DC    F'6'
A7       CSECT
         DC    F'7'
A8       CSECT
A8       RMODE 31
         DC    F'8'
A9       CSECT
A9       AMODE 31
         DC    F'9'
COMA     COM
         DS    CL12
A10      CSECT
         DS    0F
EP10     DC    F'10'
EP11     DC    F'11'
EP10     AMODE 31
EP11     RMODE ANY
         ENTRY EP10
         ENTRY EP11
         END
