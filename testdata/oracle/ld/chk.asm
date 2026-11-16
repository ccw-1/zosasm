* Caller module for the binder test: returns the number of checks
* that passed (OS linkage, called from C).
CHK      CSECT
CHK      AMODE 31
CHK      RMODE ANY
         EXTRN ADD2
         EXTRN GETTAB
         EXTRN LIBA
         WXTRN NOPE
         STM   14,12,12(13)
         LR    12,15
         USING CHK,12
         SR    6,6
* 1: call ADD2 through a V constant
         LA    1,PL
         L     15,VADD2
         BALR  14,15
         C     15,=F'30'
         BNE   N1
         LA    7,1
         OR    6,7
N1       DS    0H
* 2: GETTAB returns the table; TBLEND is 12
         L     15,VGETTAB
         BALR  14,15
         LR    5,15
         L     2,12(,5)
         C     2,=F'12'
         BNE   N2
         LA    7,2
         OR    6,7
N2       DS    0H
* 3: the V constant of LIBA equals our A constant
         L     2,20(,5)
         C     2,ACHK
         BNE   N3
         LA    7,4
         OR    6,7
N3       DS    0H
* 4: the A constant of LIBA equals the A constant to LIBA
         L     2,16(,5)
         C     2,ALIBA
         BNE   N4
         LA    7,8
         OR    6,7
N4       DS    0H
* 5: weak reference unresolved is zero (here and in LIBA)
         L     2,ANOPE
         LTR   2,2
         BNZ   N5
         L     2,24(,5)
         LTR   2,2
         BNZ   N5
         LA    7,16
         OR    6,7
N5       DS    0H
* 6: LARL to an external entry equals the A constant
         LARL  3,ADD2
         C     3,AADD2
         BNE   N6
         LA    7,32
         OR    6,7
N6       DS    0H
* 7: difference of two labels of this section, LOCTR data
         LA    2,AFTER-CHK
         L     3,ADIFF
         CR    2,3
         BNE   N7
         L     3,DATA1
         C     3,=F'5'
         BNE   N7
         LA    7,64
         OR    6,7
N7       DS    0H
* 8: second CSECT of this module, A constant and relative address
         L     2,ACSB
         LARL  3,CSB
         CR    2,3
         BNE   N8
         L     2,ACSBO
         C     2,=F'4'
         BNE   N8
         LA    7,128
         OR    6,7
N8       DS    0H
* 9: DSECT layout
         LA    5,DATA1
         USING MAPD,5
         LA    2,FLD2-FLD1
         C     2,=F'4'
         BNE   N9
         LA    2,MAPL
         C     2,=F'6'
         BNE   N9
         LA    7,256
         OR    6,7
N9       DS    0H
         DROP  5
         LR    15,6
         L     14,12(,13)
         LM    0,12,20(13)
         BR    14
AFTER    DS    0H
ADIFF    DC    A(AFTER-CHK)
VADD2    DC    V(ADD2)
VGETTAB  DC    V(GETTAB)
AADD2    DC    A(ADD2)
ALIBA    DC    A(LIBA)
ACHK     DC    A(CHK)
ANOPE    DC    A(NOPE)
ACSB     DC    A(CSB)
ACSBO    DC    A(CSBD-CSB)
PL       DC    A(P1,P2)
P1       DC    F'10'
P2       DC    F'20'
         LTORG
DATA1C   LOCTR
DATA1    DC    F'5'
         DC    H'6'
MAPD     DSECT
FLD1     DS    F
FLD2     DS    H
MAPL     EQU   *-MAPD
CSB      CSECT
CSB      AMODE 31
CSB      RMODE ANY
         DC    F'1'
CSBD     DC    F'2'
         END
