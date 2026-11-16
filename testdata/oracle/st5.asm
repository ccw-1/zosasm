* CSECT resume: LOCTR in B_TEXT and a CATTR class in between.
T1       CSECT
         DC    F'1'
L1       LOCTR
         DC    F'2'
T2       CSECT
         DC    F'3'
T1       CSECT
         DC    F'4'
C1       CATTR ALIGN(3)
         DC    F'5'
T1       CSECT
         DC    F'6'
L1       LOCTR
         DC    F'7'
C1       CATTR
         DC    F'8'
T1       CSECT
         DC    F'9'
         END
