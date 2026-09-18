// Managed 2D texture synchronization: preserve GPU execution and completion.
// Other resource types retain their original implementation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdint.h>
#include <os/log.h>
static void (*reimsOriginalResourceSync)(id,SEL,id<MTLResource>);
static void (*reimsOriginalTextureSync)(id,SEL,id<MTLTexture>,NSUInteger,NSUInteger);
static Class reimsSyncTextureClass;
static void reimsManagedTextureSync(id encoder,SEL selector,id<MTLResource>resource){
 if([resource isKindOfClass:reimsSyncTextureClass]){
  id<MTLTexture>texture=(id)resource;
  if(texture.storageMode==MTLStorageModeManaged&&texture.textureType==MTLTextureType2D&&texture.arrayLength==1){
   for(NSUInteger level=0;level<texture.mipmapLevelCount;level++)
    reimsOriginalTextureSync(encoder,@selector(synchronizeTexture:slice:level:),texture,0,level);
   static unsigned count;unsigned n=__atomic_add_fetch(&count,1,__ATOMIC_RELAXED);
   if(n<=4)os_log(OS_LOG_DEFAULT,"ReimsWeChatSync managed2D %lux%lu levels=%lu",(unsigned long)texture.width,(unsigned long)texture.height,(unsigned long)texture.mipmapLevelCount);
   return;
  }
 }
 reimsOriginalResourceSync(encoder,selector,resource);
}
static BOOL reimsInstallTextureSync(const void*nativeBase){
 if(reimsOriginalResourceSync)return YES;
 Class cls=objc_getClass("MTLIGAccelBlitCommandEncoder");
 Method generic=class_getInstanceMethod(cls,@selector(synchronizeResource:));
 Method texture=class_getInstanceMethod(cls,@selector(synchronizeTexture:slice:level:));
 if(!generic||!texture)return NO;
 Dl_info g={0},t={0};IMP gi=method_getImplementation(generic),ti=method_getImplementation(texture);
 if(!dladdr((void*)gi,&g)||!dladdr((void*)ti,&t)||g.dli_fbase!=nativeBase||t.dli_fbase!=nativeBase||
    (uintptr_t)gi-(uintptr_t)nativeBase!=0x57cb3||(uintptr_t)ti-(uintptr_t)nativeBase!=0x57ccb)return NO;
 reimsSyncTextureClass=objc_getClass("MTLIGAccelTexture");if(!reimsSyncTextureClass)return NO;
 reimsOriginalResourceSync=(void*)gi;reimsOriginalTextureSync=(void*)ti;
 method_setImplementation(generic,(IMP)reimsManagedTextureSync);return YES;
}
#ifdef REIMS_SYNC_STANDALONE
__attribute__((constructor)) static void standaloneSync(void){
 Method m=class_getInstanceMethod(objc_getClass("MTLIGAccelBlitCommandEncoder"),@selector(synchronizeTexture:slice:level:));Dl_info info={0};
 if(!m||!dladdr((void*)method_getImplementation(m),&info)||!reimsInstallTextureSync(info.dli_fbase))abort();
}
#endif
