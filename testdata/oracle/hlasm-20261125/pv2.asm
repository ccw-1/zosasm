PV2      CSECT
         USING PV2,9,10,11
         L     1,PV2+0
         L     1,PV2+4096
         L     1,PV2+5000
         L     1,PV2+8192
A        DC    F'0'
         USING 0,0
         L     2,100
         END
