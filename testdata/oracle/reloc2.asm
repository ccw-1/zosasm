RELOC    CSECT
         PRINT DATA
         EXTRN FIRST,SECOND
R1       DC    A(SECOND-FIRST)
R2       DC    A(FIRST-*)
R3       DC    A(*-FIRST)
         END
