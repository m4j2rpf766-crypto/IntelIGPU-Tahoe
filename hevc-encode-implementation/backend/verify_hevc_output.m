#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <math.h>
#include "test_fixture.h"
#include <mach-o/dyld.h>
static unsigned frames, errors;static int motion; static size_t width=1280,height=720;
static void decoded(void*a,void*b,OSStatus s,VTDecodeInfoFlags f,CVImageBufferRef image,CMTime pts,CMTime duration){
    if(s||!image){errors++;printf("decode_callback=%d image=%d\n",s,image!=NULL);return;}
    if(fabs(CMTimeGetSeconds(pts)-frames/30.0)>0.000001)errors++;
    frames++;
    if(CVPixelBufferGetWidth(image)!=width||CVPixelBufferGetHeight(image)!=height||CVPixelBufferGetPlaneCount(image)!=2){errors++;return;}
    CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    FILE*dump=NULL;if(getenv("REIMS_VERIFY_DUMP")){char path[2048];snprintf(path,sizeof(path),"%s/frame-%u.nv12",getenv("REIMS_VERIFY_DUMP"),frames);dump=fopen(path,"wb");}
    double square=0,absolute=0;unsigned maximum=0;size_t count=0;
    for(size_t p=0;p<2;p++){
        const unsigned char*base=CVPixelBufferGetBaseAddressOfPlane(image,p);
        size_t stride=CVPixelBufferGetBytesPerRowOfPlane(image,p),h=CVPixelBufferGetHeightOfPlane(image,p);
        if(dump)for(size_t y=0;y<h;y++)fwrite(base+y*stride,1,width,dump);
        for(size_t y=0;y<h;y++)for(size_t x=0;x<width;x++){
            int expected=fixture_pixel(x,y,frames-1,p,motion);
            unsigned delta=abs((int)base[y*stride+x]-expected);
            square+=delta*delta;absolute+=delta;maximum=MAX(maximum,delta);count++;
        }
    }
    CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    if(dump)fclose(dump);
    double mse=square/count,psnr=mse?10*log10(255.0*255/mse):INFINITY;
    printf("decoded=%zuX%zu pixels=%zu mae=%.6f max_error=%u psnr=%.4f\n",CVPixelBufferGetWidth(image),CVPixelBufferGetHeight(image),count,absolute/count,maximum,psnr);
    if(CVPixelBufferGetWidth(image)!=width||CVPixelBufferGetHeight(image)!=height||psnr<(motion?30:40))errors++;
}
int main(int argc,char**argv){motion=getenv("REIMS_TEST_MOTION")!=NULL;@autoreleasepool{
    if(argc<2)return 2;unsigned expected_frames=argc>2?atoi(argv[2]):1;
    NSData*data=[NSData dataWithContentsOfFile:@(argv[1])];const uint8_t*b=data.bytes;size_t n=data.length;
    NSMutableArray<NSData*>*nals=[NSMutableArray array];size_t start=SIZE_MAX;
    for(size_t i=0;i+4<=n;i++)if(!b[i]&&!b[i+1]&&!b[i+2]&&b[i+3]==1){
        if(start!=SIZE_MAX)[nals addObject:[NSData dataWithBytes:b+start length:i-start]];
        start=i+4;i+=3;
    }
    if(start!=SIZE_MAX&&start<n)[nals addObject:[NSData dataWithBytes:b+start length:n-start]];
    const uint8_t*sets[3]={0};size_t sizes[3]={0};NSMutableArray<NSData*>*samples=[NSMutableArray array];
    for(NSData*nal in nals){if(nal.length<2)return 3;unsigned type=(((const uint8_t*)nal.bytes)[0]>>1)&63;
        printf("nal_type=%u bytes=%zu\n",type,nal.length);
        if(type>=32&&type<=34){sets[type-32]=nal.bytes;sizes[type-32]=nal.length;}
        else if(type<32){NSMutableData*frame=[NSMutableData data];uint32_t len=CFSwapInt32HostToBig((uint32_t)nal.length);[frame appendBytes:&len length:4];[frame appendData:nal];[samples addObject:frame];}
    }
    if(!sets[0]||!sets[1]||!sets[2]||!samples.count)return 4;
    CMVideoFormatDescriptionRef format=NULL;OSStatus s=CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL,3,sets,sizes,4,NULL,&format);
    printf("format=%d\n",s);if(s)return 5;CMVideoDimensions dimensions=CMVideoFormatDescriptionGetDimensions(format);width=dimensions.width;height=dimensions.height;
    VTDecompressionOutputCallbackRecord callback={decoded,NULL};VTDecompressionSessionRef session=NULL;
    NSDictionary*spec=@{(__bridge NSString*)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder:@NO,@"DecoderID":@"com.apple.videotoolbox.videodecoder.hevc.vcp",@"EnableSandboxedVideoDecoder":@NO};
    NSDictionary*attrs=@{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    s=VTDecompressionSessionCreate(NULL,format,(__bridge CFDictionaryRef)spec,(__bridge CFDictionaryRef)attrs,&callback,&session);
    printf("decoder_create=%d\n",s);if(s)return 6;
    CFTypeRef hardware=NULL;s=VTSessionCopyProperty(session,kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,NULL,&hardware);
    printf("hardware_disabled_requested=1 decoder_hardware=%s property=%d\n",s?"unavailable":(hardware==kCFBooleanTrue?"true":"false"),s);if(hardware==kCFBooleanTrue)return 7;if(hardware)CFRelease(hardware);
    unsigned input_index=0;
    for(NSData*frame in samples){
    CMBlockBufferRef block=NULL;CMSampleBufferRef sample=NULL;size_t length=frame.length;
    s=CMBlockBufferCreateWithMemoryBlock(NULL,NULL,length,NULL,NULL,0,length,0,&block);
    if(!s)s=CMBlockBufferReplaceDataBytes(frame.bytes,block,0,length);
    CMSampleTimingInfo timing={CMTimeMake(1,30),CMTimeMake(input_index++,30),kCMTimeInvalid};
    if(!s)s=CMSampleBufferCreateReady(NULL,block,format,1,1,&timing,1,&length,&sample);
    if(!s)s=VTDecompressionSessionDecodeFrame(session,sample,0,NULL,NULL);
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    if(sample)CFRelease(sample);if(block)CFRelease(block);if(s){errors++;break;}
    }
    for(uint32_t i=0;i<_dyld_image_count();i++){const char*n=_dyld_get_image_name(i);if(strstr(n,"HEVC")||strstr(n,"GraphicsVA"))printf("decoder_image=%s\n",n);}
    printf("decode_status=%d frames=%u errors=%u\n",s,frames,errors);
    VTDecompressionSessionInvalidate(session);CFRelease(session);CFRelease(format);
    return s||frames!=expected_frames||errors?8:0;
}}
