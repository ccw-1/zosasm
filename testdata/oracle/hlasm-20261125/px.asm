PX       CSECT
WORK     DXD   CL100,F
EXTRA    DXD   D
         DC    Q(WORK)
         DC    QD(WORK)
         DC    QY(WORK)
         DC    J(WORK)
         DC    JD(WORK)
         CXD
         EXTRN FUNC
         DC    R(FUNC)
         DC    RD(FUNC)
         END
