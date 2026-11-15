*        THINGS THAT ARE NOT CONVERTED
         COPY  MYREGS
         MACRO
&LAB     SETUP &A
&LAB     LA    1,&A
         MEND
T1       CSECT
         SETUP 5
         AIF   (&X).SKIP
         LR    R1,R2
.SKIP    ANOP
         SAVE  (14,12)
         END
