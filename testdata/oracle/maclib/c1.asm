T        CSECT
* A COMMENT WITH A MARK                                                X
               LR    1,2 THIS IS COMMENT TEXT
* SECOND                                                               X
ABCDEFGHIJKLMN LR 5,6 BAD COLS                                         X
               LR    7,8 ALSO COMMENT
         LR    9,10 REAL
         MACRO
         MM
.* MAC COMMENT                                                         X
               MNOTE 0,'IN COMMENT'
         MNOTE 0,'MM RAN'
         MEND
         MM
         END
