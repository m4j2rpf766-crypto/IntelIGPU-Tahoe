// Tahoe 25G83 -> the seven audited TGL render descriptor consumers only.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

static IMP reimsNativeRPPrivate;
static const void *reimsTGLImage;
static char reimsLegacyRPKey, reimsLegacyAttachmentKey;
static IMP reimsNativeAttachmentPrivate;
static BOOL reimsRPSupportedCaller(const void *address){
 Dl_info info={0};if(!dladdr(address,&info)||info.dli_fbase!=reimsTGLImage)return NO;
 uintptr_t off=(uintptr_t)address-(uintptr_t)info.dli_fbase;
 switch(off){case 0x7120:case 0x7fde:case 0x886c:case 0x9318:case 0x9950:case 0xa283:case 0xbcbd:return YES;default:return NO;}
}
// This is a read-only projection of the prefix actually consumed by those
// callers. The caller's descriptor continues owning all referenced objects.
static void reimsProjectRP(const uint8_t *native,uint8_t old[0x108],BOOL coverage,BOOL one){
 memcpy(old,native,0xa8);
 memcpy(old+0xa8,native+0xb8,0x28);
 uint64_t flags;memcpy(&flags,native+0xe0,8);
 flags=((flags>>2)&((UINT64_C(1)<<34)-4)) | (coverage?1:0) | (one?2:0);
 memcpy(old+0xd0,&flags,8);
 memcpy(old+0xd8,native+0xe8,0x30);
}
__attribute__((noinline)) static const void *reimsRPPrivate(id self,SEL selector){
 const void *caller=__builtin_return_address(0);
 const uint8_t *native=((const void *(*)(id,SEL))reimsNativeRPPrivate)(self,selector);
 if(!native||!reimsRPSupportedCaller(caller))return native;
 uint8_t projected[0x108];
 MTLRenderPipelineDescriptor *d=self;
 reimsProjectRP(native,projected,d.alphaToCoverageEnabled,d.alphaToOneEnabled);
 @synchronized(self){
  NSData *current=objc_getAssociatedObject(self,&reimsLegacyRPKey);
  if(!current||current.length!=sizeof(projected)||memcmp(current.bytes,projected,sizeof(projected))){
   current=[NSData dataWithBytes:projected length:sizeof(projected)];
   objc_setAssociatedObject(self,&reimsLegacyRPKey,current,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  // Keep a replaced immutable snapshot alive to the caller's autorelease pool;
  // never overwrite bytes a simultaneous compiler reader may be using.
  [[current retain] autorelease];
  return current.bytes;
 }
}
// TGL blend generation consumes the attachment bits at return PC 0x7907.
__attribute__((noinline)) static const void *reimsAttachmentPrivate(id self,SEL selector){
 const void *caller=__builtin_return_address(0);Dl_info info={0};
 const void *native=((const void *(*)(id,SEL))reimsNativeAttachmentPrivate)(self,selector);
 if(!native||!dladdr(caller,&info)||info.dli_fbase!=reimsTGLImage||
    (uintptr_t)caller-(uintptr_t)info.dli_fbase!=0x7907)return native;
 MTLRenderPipelineColorAttachmentDescriptor *a=self;uint64_t newer;memcpy(&newer,native,8);
 uint64_t old=(a.blendingEnabled?1ull:0ull)|((uint64_t)a.rgbBlendOperation<<1)|
  ((uint64_t)a.alphaBlendOperation<<4)|((uint64_t)a.sourceRGBBlendFactor<<7)|
  ((uint64_t)a.sourceAlphaBlendFactor<<12)|((uint64_t)a.destinationRGBBlendFactor<<17)|
  ((uint64_t)a.destinationAlphaBlendFactor<<22)|(((uint64_t)a.writeMask&15)<<27)|
  (((newer>>33)&31)<<31)|((uint64_t)a.pixelFormat<<36);
 @synchronized(self){
  NSData *current=objc_getAssociatedObject(self,&reimsLegacyAttachmentKey);
  if(!current||memcmp(current.bytes,&old,8)){
   current=[NSData dataWithBytes:&old length:8];
   objc_setAssociatedObject(self,&reimsLegacyAttachmentKey,current,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  [[current retain] autorelease];return current.bytes;
 }
}
static BOOL reimsInstallRPCompat(const void *tglImage){
 if(reimsNativeRPPrivate)return reimsTGLImage==tglImage;
 Class c=objc_getClass("MTLRenderPipelineDescriptorInternal");SEL s=sel_registerName("_descriptorPrivate");
 Method m=class_getInstanceMethod(c,s);Ivar p=class_getInstanceVariable(c,"_private");
 if(!m||!p||ivar_getOffset(p)!=0x30||class_getInstanceSize(c)!=0x338)return NO;
 const char *encoding=ivar_getTypeEncoding(p);
 if(!encoding||!strstr(encoding,"\"shaderValidationState\"q\"textureWriteRoundingMode\"q")||
    !strstr(encoding,"\"alphaToCoverageEnabled\"b2\"alphaToOneEnabled\"b2"))return NO;
 Class ac=objc_getClass("MTLRenderPipelineColorAttachmentDescriptorInternal");
 Method am=class_getInstanceMethod(ac,s);Ivar ap=class_getInstanceVariable(ac,"_private");
 const char *ae=ap?ivar_getTypeEncoding(ap):NULL;
 if(!am||!ap||ivar_getOffset(ap)!=8||class_getInstanceSize(ac)!=16||!ae||
    !strstr(ae,"\"blendingEnabled\"b2")||!strstr(ae,"\"writeMask\"b5\"logicOpEnabled\"b1\"logicOp\"b4\"pixelFormat\"b22"))return NO;
 // Dynamic probe verifies pointer and scalar slots in this exact host layout.
 MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
 MTLVertexDescriptor *v=[MTLVertexDescriptor vertexDescriptor];d.vertexDescriptor=v;d.rasterSampleCount=4;
 IMP original=method_getImplementation(m);
 const uint8_t *raw=((const void *(*)(id,SEL))original)(d,s);
 uint64_t count=0;void *vertex=NULL;memcpy(&count,raw+0xb8,8);memcpy(&vertex,raw+0x110,8);
 BOOL ok=count==4&&vertex==d.vertexDescriptor;[d release];if(!ok)return NO;
 reimsTGLImage=tglImage;reimsNativeRPPrivate=original;
 reimsNativeAttachmentPrivate=method_getImplementation(am);
 method_setImplementation(am,(IMP)reimsAttachmentPrivate);
 method_setImplementation(m,(IMP)reimsRPPrivate);
 return YES;
}
