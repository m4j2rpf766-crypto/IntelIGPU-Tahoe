#import "metal_entries.m"
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#import "pipeline_descriptor_compat.m"

@interface MTLIOAccelDevice : NSObject @end
@interface ReimsTahoeMetalDevice : MTLIOAccelDevice @end
static BOOL uuidMatches(const struct mach_header_64 *h,const uint8_t wanted[16]){
 if(!h||h->magic!=MH_MAGIC_64)return NO;
 const uint8_t *cursor=(const uint8_t*)(h+1),*end=cursor+h->sizeofcmds;
 for(uint32_t i=0;i<h->ncmds;i++){
  if(cursor+sizeof(struct load_command)>end)return NO;
  const struct load_command*c=(const void*)cursor;
  if(c->cmdsize<8||cursor+c->cmdsize>end)return NO;
  if(c->cmd==LC_UUID&&c->cmdsize>=sizeof(struct uuid_command))return !memcmp(((const struct uuid_command*)c)->uuid,wanted,16);
  cursor+=c->cmdsize;
 }
 return NO;
}
// No allocation or user-client open; suitable for metadata-only candidate audit.
BOOL ReimsInstallMetalEntryCandidate(const char *path){
 @synchronized([ReimsTahoeMetalDevice class]){
  static void *nativeHandle;
  static const uint8_t tglUUID[]={0x2b,0x84,0x9e,0x57,0xac,0x6a,0x32,0x16,0xa6,0x7b,0x38,0xe0,0x80,0x60,0x35,0x85};
  static const uint8_t metalUUID[]={0x5d,0x64,0xfa,0x80,0x29,0xce,0x32,0xaa,0xba,0xb6,0x4e,0x50,0x34,0x13,0x2c,0x0b};
  BOOL host=NO;
  for(uint32_t i=0;i<_dyld_image_count();i++){
   const char *name=_dyld_get_image_name(i),*leaf=strrchr(name,'/');
   if(leaf&&!strcmp(leaf+1,"Metal"))host=uuidMatches((const void*)_dyld_get_image_header(i),metalUUID);
  }
  if(!host||!path)return NO;
  if(!nativeHandle)nativeHandle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
  if(!nativeHandle)return NO;
  Class device=objc_getClass("MTLIGAccelDevice"),buffer=objc_getClass("MTLIGAccelBuffer"),texture=objc_getClass("MTLIGAccelTexture");
  Class classes[]={device,buffer,texture};
  const char *anchors[]={"initWithAcceleratorPort:","resourceIndex","newTextureViewWithPixelFormat:"};
  for(unsigned i=0;i<3;i++){
   Method anchor=class_getInstanceMethod(classes[i],sel_registerName(anchors[i]));Dl_info info={0};
   if(!anchor||!dladdr((void*)method_getImplementation(anchor),&info)||!uuidMatches(info.dli_fbase,tglUUID))return NO;
  }
  void *metal=dlopen("/System/Library/Frameworks/Metal.framework/Metal",RTLD_NOW|RTLD_LOCAL);
  const uint64_t *sentinel=metal?dlsym(metal,"_MTLInvalidResourceIndex"):NULL;
  Dl_info nativeInfo={0};
  Method nativeAnchor=class_getInstanceMethod(device,sel_registerName("initWithAcceleratorPort:"));
  BOOL result=sentinel&&nativeAnchor&&dladdr((void*)method_getImplementation(nativeAnchor),&nativeInfo)&&
    reimsInstallRPCompat(nativeInfo.dli_fbase)&&installEntries(device,buffer,texture,*sentinel);
  if(metal)dlclose(metal);
  return result;
 }
}
@implementation ReimsTahoeMetalDevice
+ (id)allocWithZone:(NSZone*)zone {
 // Principal-class factory: patch synchronously before returning any native
 // device. No background installer and no race with the first initialization.
 NSBundle*b=[NSBundle bundleForClass:self];
 NSString*path=[[b executablePath] stringByDeletingLastPathComponent];
 path=[path stringByAppendingPathComponent:@"AppleIntelTGLGraphicsMTLDriver"];
 if(!ReimsInstallMetalEntryCandidate(path.fileSystemRepresentation)){
  os_log_error(OS_LOG_DEFAULT,"ReimsMetalEntry candidate ABI preflight failed; device allocation refused");return nil;
 }
 if(!strcmp(getprogname(),"WindowServer")){
  static dispatch_once_t timingOnce;
  dispatch_once(&timingOnce, ^{
   NSString *timing=[[[b executablePath] stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"libWSTimingTahoe.dylib"];
   if(!dlopen(timing.fileSystemRepresentation,RTLD_NOW|RTLD_LOCAL))os_log_error(OS_LOG_DEFAULT,"ReimsWSTiming load failed");
  });
 }
 return [(id)installedDevice allocWithZone:zone];
}
@end
