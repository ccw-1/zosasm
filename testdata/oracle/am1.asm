* AMODE/RMODE defaults for one-sided specifications.
R24      CSECT
R24      RMODE 24
         DC    F'1'
R31      CSECT
R31      RMODE 31
         DC    F'1'
RAN      CSECT
RAN      RMODE ANY
         DC    F'1'
R64      CSECT
R64      RMODE 64
         DC    F'1'
A24      CSECT
A24      AMODE 24
         DC    F'1'
A31      CSECT
A31      AMODE 31
         DC    F'1'
AAN      CSECT
AAN      AMODE ANY
         DC    F'1'
A64      CSECT
A64      AMODE 64
         DC    F'1'
B2       CSECT
B2       AMODE 31
B2       RMODE 24
         DC    F'1'
B3       CSECT
B3       AMODE ANY
B3       RMODE 24
         DC    F'1'
         END
