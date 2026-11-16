S0       CSECT
         DC    F'0'
S1       CSECT
C1       CATTR
         DC    F'1'
S2       CSECT
C2       CATTR ALIGN(0)
         DC    F'2'
S3       CSECT
C3       CATTR ALIGN(12)
         DC    F'3'
S4       CSECT
C4       CATTR DEFLOAD
         DC    F'4'
S5       CSECT
C5       CATTR EXECUTABLE
         DC    F'5'
S6       CSECT
C6       CATTR NOTEXECUTABLE
         DC    F'6'
S7       CSECT
C7       CATTR MOVABLE
         DC    F'7'
S8       CSECT
C8       CATTR NOLOAD
         DC    F'8'
S9       CSECT
C9       CATTR NOTREUS
         DC    F'9'
S10      CSECT
C10      CATTR READONLY
         DC    F'10'
S12      CSECT
C12      CATTR REMOVABLE
         DC    F'12'
S13      CSECT
C13      CATTR RENT
         DC    F'13'
S14      CSECT
C14      CATTR REUS
         DC    F'14'
S15      CSECT
C15      CATTR RMODE(24)
         DC    F'15'
S16      CSECT
C16      CATTR RMODE(31)
         DC    F'16'
S17      CSECT
C17      CATTR RMODE(64)
         DC    F'17'
S18      CSECT
C18      CATTR RMODE(ANY)
         DC    F'18'
S19      CSECT
C19      CATTR PART(P19)
         DC    F'19'
S20      CSECT
C20      CATTR PART(P20),FILL(7)
         DC    F'20'
S21      CSECT
C21      CATTR PART(P21),PRIORITY(5)
         DC    F'21'
S22      CSECT
C22      CATTR RENT,READONLY,ALIGN(2)
         DC    F'22'
         END
