#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <time.h>
#include <math.h>
#include "test_fixture.h"
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static FILE*outFile;static unsigned emitted,errors;
static void encoded(void*ref,void*source,OSStatus status,VTEncodeInfoFlags flags,CMSampleBufferRef sb){
 printf("callback status=%d flags=%u ready=%d\n",status,(unsigned)flags,sb?CMSampleBufferDataIsReady(sb):0);
 if(status||!sb||!CMSampleBufferDataIsReady(sb)){errors++;return;}
 CMTime pts=CMSampleBufferGetPresentationTimeStamp(sb);if(!CMTIME_IS_NUMERIC(pts)||fabs(CMTimeGetSeconds(pts)-emitted/30.0)>0.000001){errors++;return;}
 CMFormatDescriptionRef fmt=CMSampleBufferGetFormatDescription(sb);size_t count=0,len=0;const uint8_t*p=NULL;int nh=0;
 OSStatus r=CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt,0,&p,&len,&count,&nh);unsigned char start[4]={0,0,0,1};
 if(r||nh<1||nh>4){errors++;return;}
 if(!emitted)for(size_t i=0;i<count;i++){r=CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt,i,&p,&len,NULL,NULL);if(r){errors++;return;}fwrite(start,1,4,outFile);fwrite(p,1,len,outFile);}
 CMBlockBufferRef bb=CMSampleBufferGetDataBuffer(sb);size_t total=CMBlockBufferGetDataLength(bb);uint8_t*d=malloc(total);r=CMBlockBufferCopyDataBytes(bb,0,total,d);if(r){free(d);errors++;return;}
 size_t pos=0;while(pos+(size_t)nh<=total){uint32_t n=0;for(int j=0;j<nh;j++)n=(n<<8)|d[pos++];if(n>total-pos){errors++;break;}fwrite(start,1,4,outFile);fwrite(d+pos,1,n,outFile);pos+=n;}
 if(pos!=total)errors++;free(d);emitted++;printf("emitted=%u bytes=%zu pts=%.6f\n",emitted,total,CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb)));fflush(outFile);
}
int main(int argc,char**argv){unsigned width=getenv("REIMS_TEST_WIDTH")?atoi(getenv("REIMS_TEST_WIDTH")):1280;unsigned height=getenv("REIMS_TEST_HEIGHT")?atoi(getenv("REIMS_TEST_HEIGHT")):720;unsigned bitrate=getenv("REIMS_TEST_BITRATE")?atoi(getenv("REIMS_TEST_BITRATE")):0;setvbuf(stdout,NULL,_IONBF,0);@autoreleasepool{
 BOOL registered=!strcmp(argv[1],"native")||!strcmp(argv[1],"registered")||!strcmp(argv[1],"automatic");
 NSString*eid=!strcmp(argv[1],"native")?@"com.apple.videotoolbox.videoencoder.hevc.gva":registered?@"lab.reims.videotoolbox.videoencoder.hevc":@"lab.reims.hevc.path-test";
 if(!registered){
 void*h=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!h){puts(dlerror());return 1;}
 void*factory=dlsym(h,"HEVCEncoder_CreateInstance");
 int(*reg)(unsigned,CFDictionaryRef,void*)=dlsym(RTLD_DEFAULT,"VTRegisterVideoEncoderWithInfo");if(!reg||!factory)return 2;
 NSDictionary*info=@{@"CMClassImplementationID":eid,@"VTEncoderName":@"TGL HEVC isolated test",@"VTIsHardwareAccelerated":@YES};
 printf("register=%d\n",reg(kCMVideoCodecType_HEVC,(__bridge CFDictionaryRef)info,factory));
 }else{CFArrayRef list=NULL;OSStatus ls=VTCopyVideoEncoderList(NULL,&list);NSLog(@"encoder_list status=%d %@",ls,list);if(list)CFRelease(list);}
 /* No client-side Metal setup: VT alone starts the real encoder service. */
 void(*install)(void)=dlsym(RTLD_DEFAULT,"diag_install");if(install)install();
 NSMutableDictionary*spec=[@{(__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder:@YES,@"EncoderID":eid} mutableCopy];
 if(!strcmp(argv[1],"automatic"))[spec removeObjectForKey:@"EncoderID"];
 NSDictionary*attrs=@{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),(__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey:@{}};
 VTCompressionSessionRef session=NULL;OSStatus r=VTCompressionSessionCreate(NULL,width,height,kCMVideoCodecType_HEVC,(__bridge CFDictionaryRef)spec,(__bridge CFDictionaryRef)attrs,NULL,argc>2?encoded:NULL,NULL,&session);
 printf("session_create=%d session=%p\n",r,session);
 if(session){CFTypeRef hw=NULL;r=VTSessionCopyProperty(session,kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,NULL,&hw);printf("hardware_status=%d true=%d\n",r,hw==kCFBooleanTrue);if(hw!=kCFBooleanTrue)r=-1;if(hw)CFRelease(hw);
 if(argc>2&&r==0){outFile=fopen(argv[2],"wb");if(!outFile)return 4;
  NSMutableDictionary*props=[@{(__bridge NSString*)kVTCompressionPropertyKey_ProfileLevel:(__bridge NSString*)kVTProfileLevel_HEVC_Main_AutoLevel,(__bridge NSString*)kVTCompressionPropertyKey_Quality:@0.65,(__bridge NSString*)kVTCompressionPropertyKey_ExpectedFrameRate:@30,(__bridge NSString*)kVTCompressionPropertyKey_AllowFrameReordering:@NO} mutableCopy];
  if(bitrate){[props removeObjectForKey:(__bridge NSString*)kVTCompressionPropertyKey_Quality];props[(__bridge NSString*)kVTCompressionPropertyKey_AverageBitRate]=@(bitrate);}
  printf("input=%ux%u bitrate=%u\n",width,height,bitrate);
  r=VTSessionSetProperties(session,(__bridge CFDictionaryRef)props);printf("set_properties=%d\n",r);
  if(!r){r=VTCompressionSessionPrepareToEncodeFrames(session);printf("prepare=%d\n",r);}
  int frames=argc>3?atoi(argv[3]):1;
  const int motion=getenv("REIMS_TEST_MOTION")!=NULL;
  double encodeStart=now();
  for(int f=0;!r&&f<frames;f++){CVPixelBufferRef pb=NULL;r=CVPixelBufferCreate(NULL,width,height,kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,(__bridge CFDictionaryRef)attrs,&pb);if(r)break;
   CVPixelBufferLockBaseAddress(pb,0);for(size_t plane=0;plane<2;plane++){uint8_t*base=CVPixelBufferGetBaseAddressOfPlane(pb,plane);size_t stride=CVPixelBufferGetBytesPerRowOfPlane(pb,plane),height=CVPixelBufferGetHeightOfPlane(pb,plane);for(size_t y=0;y<height;y++)for(size_t x=0;x<width;x++)base[y*stride+x]=fixture_pixel(x,y,f,plane,motion);}if(f==0){void(*input)(unsigned,void*)=dlsym(RTLD_DEFAULT,"diag_input");if(input)input(f,CVPixelBufferGetIOSurface(pb));}CVPixelBufferUnlockBaseAddress(pb,0);
   VTEncodeInfoFlags flags=0;r=VTCompressionSessionEncodeFrame(session,pb,CMTimeMake(f,30),CMTimeMake(1,30),NULL,NULL,&flags);printf("encode_frame=%d status=%d flags=%u\n",f,r,(unsigned)flags);CVPixelBufferRelease(pb);
  }
  OSStatus drain=VTCompressionSessionCompleteFrames(session,kCMTimeInvalid);printf("drain=%d emitted=%u errors=%u\n",drain,emitted,errors);double seconds=now()-encodeStart;printf("timing frames=%u seconds=%.9f fps=%.3f\n",emitted,seconds,emitted/seconds);if(!r)r=drain;if(!r&&(errors||emitted!=(unsigned)frames))r=-1;fclose(outFile);
 }
 void(*finish)(const char*)=dlsym(RTLD_DEFAULT,"diag_finish");if(finish&&getenv("REIMS_DIAG_DIR"))finish(getenv("REIMS_DIAG_DIR"));
 VTCompressionSessionInvalidate(session);CFRelease(session);}
 return r?3:0;
}}
