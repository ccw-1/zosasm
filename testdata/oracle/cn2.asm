* CNOP gaps of 6 and 8 bytes.
CN2      CSECT
         DC    H'1'
         CNOP  0,8
         DC    X'22'
         DS    0D
         DC    H'1'
         CNOP  2,8
         DC    X'33'
         DS    0D
         CNOP  0,8
         CNOP  0,8
         DC    X'44'
         DS    0D
         DC    H'0'
         CNOP  0,16
         DC    X'55'
         END
