/* Compare an independent decoder's tightly packed yuv420p output to the source.
 * PSNR is reported as a quality metric, not proof of bitstream correctness. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "test_fixture.h"
int main(int argc,char **argv){
 if(argc!=5)return 2;
 unsigned w=atoi(argv[1]),h=atoi(argv[2]),expected=atoi(argv[3]),motion=atoi(argv[4]);
 if(!w||!h||(w&1)||(h&1)||!expected)return 2;
 size_t bytes=(size_t)w*h*3/2;unsigned char *buf=malloc(bytes);if(!buf)return 2;
 unsigned frame=0;double total=0,worst=INFINITY;int failed=0;
 for(;;){
  size_t got=fread(buf,1,bytes,stdin);if(!got)break;
  if(got!=bytes){fprintf(stderr,"truncated_frame bytes=%zu expected=%zu\n",got,bytes);failed=1;break;}
  if(frame>=expected){failed=1;break;}
  double square=0,absolute=0;unsigned maximum=0;
  for(unsigned p=0;p<3;p++){
   unsigned pw=p?w/2:w,ph=p?h/2:h;
   const unsigned char *base=buf+(p?(size_t)w*h+(p-1)*(size_t)w*h/4:0);
   for(unsigned y=0;y<ph;y++)for(unsigned x=0;x<pw;x++){
    int original=fixture_pixel(p?x*2+(p==2):x,y,frame,p!=0,motion);
    unsigned delta=abs((int)base[(size_t)y*pw+x]-original);
    square+=(double)delta*delta;absolute+=delta;if(delta>maximum)maximum=delta;
   }
  }
  double psnr=square?10*log10(65025.0*bytes/square):INFINITY;
  if(psnr<worst)worst=psnr;total+=square;
  printf("frame=%u mae=%.6f max_error=%u psnr=%.4f\n",++frame,absolute/bytes,maximum,psnr);
 }
 if(ferror(stdin)||frame!=expected)failed=1;
 printf("frames=%u expected=%u framing_errors=%d minimum_psnr=%.4f aggregate_psnr=%.4f\n",frame,expected,failed,worst,total?10*log10(65025.0*bytes*frame/total):INFINITY);
 free(buf);return failed?1:0;
}
