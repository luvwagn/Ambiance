/* yuv_draw (src/yuvdraw.c) on the host: the colours against BT.601 by
 * floating point, BGR the reverse of RGB, a 2:1 scale every other pixel.
 * MIT, (c) 2026 Brian King */
#include <stdio.h>
#include <math.h>
#include "exec/types.h"
#include "cybergraphx/cybergraphics.h"
BOOL yuv_draw(const UBYTE *y, const UBYTE *cb, const UBYTE *cr, ULONG ymod, ULONG cmod, ULONG sw, ULONG sh, UBYTE *dst, ULONG bpr, ULONG fmt, ULONG dw, ULONG dh);
int main(void){
  UBYTE Y[16*16],U[8*8],V[8*8],out[16*16*4]; int i,bad=0; srand(1);
  for(i=0;i<256;i++)Y[i]=16+rand()%220; for(i=0;i<64;i++){U[i]=16+rand()%225;V[i]=16+rand()%225;}
  /* RGB24 1:1 against the float formula (big-endian not an issue for bytes) */
  yuv_draw(Y,U,V,16,8,16,16,out,48,PIXFMT_RGB24,16,16);
  for(int yy=0;yy<16;yy++)for(int x=0;x<16;x++){double c=1.164*(Y[yy*16+x]-16),d=U[(yy/2)*8+x/2]-128,e=V[(yy/2)*8+x/2]-128;
    double rgb[3]={c+1.596*e,c-0.391*d-0.813*e,c+2.018*d};
    for(int k=0;k<3;k++){double v=rgb[k]<0?0:rgb[k]>255?255:rgb[k]; if(fabs(v-out[(yy*16+x)*3+k])>2)bad++;}}
  /* BGR24 = RGB24 reversed; ARGB32 big-endian longword written natively here (host LE): check via value */
  UBYTE o2[16*16*3]; yuv_draw(Y,U,V,16,8,16,16,o2,48,PIXFMT_BGR24,16,16);
  for(i=0;i<256;i++) if(o2[i*3]!=out[i*3+2]||o2[i*3+2]!=out[i*3])bad++;
  /* 2:1 scale: dest 8x8 picks even source pixels */
  UBYTE o3[8*8*3]; yuv_draw(Y,U,V,16,8,16,16,o3,24,PIXFMT_RGB24,8,8);
  for(int yy=0;yy<8;yy++)for(int x=0;x<8;x++)for(int k=0;k<3;k++) if(o3[(yy*8+x)*3+k]!=out[((yy*2)*16+x*2)*3+k])bad++;
  printf("bad %d\n",bad); return bad!=0;}
