#include <stdio.h>
int CHK(void);
#pragma linkage(CHK,OS)
int main(void){ int r=CHK(); printf("CHK=%d (want 511)\n",r); return r==511?0:1; }
