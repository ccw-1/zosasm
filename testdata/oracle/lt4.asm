LT4      CSECT
         USING LT4,15
HERE     L     1,=3A(*)
         L     1,=A(*)
         L     1,=A(*)
         L     1,=A(*+4,2*3)
         L     1,=A(2*3)
         L     1,=C'*'
         L     1,=A(*-HERE)
         L     1,=A(HERE)
         L     1,=A(HERE)
         END
