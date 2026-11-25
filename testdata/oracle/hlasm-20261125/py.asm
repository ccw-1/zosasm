PY       CSECT
X        DC    F'1'
PY2      CSECT
Y        DC    F'2'
PY3      CSECT
         DC    A(X-Y)
         DC    A(X-*)
         DC    A(*-Y)
         END
