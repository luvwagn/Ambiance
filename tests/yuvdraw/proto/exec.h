#include <string.h>
#define AllocVec(n,f) calloc(1, n)
#define FreeVec(p) free(p)
#define CopyMem(s_,d_,n_) memcpy((d_),(s_),(n_))
#define MEMF_ANY 0
#define MEMF_CLEAR 0x10000
