#ifndef MACROS_INC
#define MACROS_INC
* macros.inc - shared definitions
#define PAGE      4096
#define SAVE(n)   DC    18F ## #(n)
#define STR(x)    C ## #(x)
#define ZERO(r)   XR    r,r
#endif
