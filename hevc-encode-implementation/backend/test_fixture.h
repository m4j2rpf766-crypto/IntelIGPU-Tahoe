#ifndef REIMS_TEST_FIXTURE_H
#define REIMS_TEST_FIXTURE_H
#include <stdint.h>
/* Deterministic moving spatial texture, independently checked after decoding. */
static inline uint8_t fixture_pixel(unsigned x,unsigned y,unsigned frame,unsigned plane,int motion){
 if(!motion)return plane?(x%2?144:112):16+((x/16+y/16+frame*3)%200);
 unsigned xx=x+frame*5,yy=y+frame*3;
 if(plane)return x%2?96+((yy/16+frame)%64):96+((xx/16+frame*2)%64);
 unsigned texture=((xx/8)*1103515245u)^((yy/8)*2654435761u);
 unsigned gradient=(xx/3+yy/3)%128;
 return 16+((gradient+(texture>>27)*3)%220);
}
#endif
