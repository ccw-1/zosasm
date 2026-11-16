#include <stdio.h>
int XB(void);
#pragma linkage(XB,OS)
int main(void){ int r=XB(); printf("XB=%d (want 31)\n",r); return r==31?0:1; }
