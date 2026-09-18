// Candidate only: install before creating the first TGL device in this process.
// Does not create a device, change resource layouts or implement indexed heaps.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <os/log.h>

typedef struct {uint8_t red,green,blue,alpha;} ReimsSwizzle;
static IMP legacyViews[6],legacyBuffers[2];
static uint64_t invalidIndex;
static Class installedDevice,installedBuffer,installedTexture;
static __thread const char *lastEntryError;
const char *ReimsMetalEntryLastError(void){return lastEntryError;}
static void unsupported(SEL selector,const char *reason){
 lastEntryError=reason;errno=ENOTSUP;
 os_log_error(OS_LOG_DEFAULT,"ReimsMetalEntry %{public}s: %{public}s",sel_getName(selector),reason);
}
static BOOL indexAllowed(SEL selector,uint64_t index){
 lastEntryError=NULL;
 if(index==invalidIndex)return YES;
 unsupported(selector,"explicit resource index uses a different TGL heap encoding; unsupported");return NO;
}
static id initFormat(id self,SEL cmd,id source,NSUInteger format,uint64_t index){
 if(!indexAllowed(cmd,index)){[self release];return nil;}
 return ((id(*)(id,SEL,id,NSUInteger))legacyViews[0])(self,sel_registerName("initWithTexture:pixelFormat:"),source,format);
}
static id initRange(id self,SEL cmd,id source,NSUInteger format,NSUInteger type,NSRange levels,NSRange slices,uint64_t index){
 if(!indexAllowed(cmd,index)){[self release];return nil;}
 return ((id(*)(id,SEL,id,NSUInteger,NSUInteger,NSRange,NSRange))legacyViews[1])(self,sel_registerName("initWithTexture:pixelFormat:textureType:levels:slices:"),source,format,type,levels,slices);
}
static id initSwizzle(id self,SEL cmd,id source,NSUInteger format,NSUInteger type,NSRange levels,NSRange slices,ReimsSwizzle swizzle,uint64_t index){
 if(!indexAllowed(cmd,index)){[self release];return nil;}
 return ((id(*)(id,SEL,id,NSUInteger,NSUInteger,NSRange,NSRange,ReimsSwizzle))legacyViews[2])(self,sel_registerName("initWithTexture:pixelFormat:textureType:levels:slices:swizzle:"),source,format,type,levels,slices,swizzle);
}
static id viewFormat(id self,SEL cmd,NSUInteger format,uint64_t index){
 if(!indexAllowed(cmd,index))return nil;
 return ((id(*)(id,SEL,NSUInteger))legacyViews[3])(self,sel_registerName("newTextureViewWithPixelFormat:"),format);
}
static id viewRange(id self,SEL cmd,NSUInteger format,NSUInteger type,NSRange levels,NSRange slices,uint64_t index){
 if(!indexAllowed(cmd,index))return nil;
 return ((id(*)(id,SEL,NSUInteger,NSUInteger,NSRange,NSRange))legacyViews[4])(self,sel_registerName("newTextureViewWithPixelFormat:textureType:levels:slices:"),format,type,levels,slices);
}
static id viewSwizzle(id self,SEL cmd,NSUInteger format,NSUInteger type,NSRange levels,NSRange slices,ReimsSwizzle swizzle,uint64_t index){
 if(!indexAllowed(cmd,index))return nil;
 return ((id(*)(id,SEL,NSUInteger,NSUInteger,NSRange,NSRange,ReimsSwizzle))legacyViews[5])(self,sel_registerName("newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:"),format,type,levels,slices,swizzle);
}
static id rejectGlobal(id self,SEL cmd){(void)self;unsupported(cmd,"compute ICB global memory is not implemented");return nil;}
static id rejectGlobalHeaderArgument(id self,SEL cmd,void *header){(void)header;return rejectGlobal(self,cmd);}
static void *rejectGlobalHeader(id self,SEL cmd){(void)rejectGlobal(self,cmd);return NULL;}
static void rejectVoid(id self,SEL cmd){
 (void)self;unsupported(cmd,"compute ICB/indirection operation is not implemented");
 [NSException raise:@"ReimsUnsupportedMetalEntry" format:@"%s: %s",sel_getName(cmd),lastEntryError];
}
static void rejectIndirection(id self,SEL cmd,id resource,uint64_t index){(void)resource;(void)index;rejectVoid(self,cmd);}
// TGL16 has no binary archive implementation/targetDeviceArchitecture.
// Reject through the device API before Tahoe's generic loader asserts on nil.
static id rejectBinaryArchive(id self,SEL cmd,id descriptor,NSError **error){
 (void)self;(void)descriptor;
 unsupported(cmd,"binary archives are not implemented by this TGL adapter; compile the original AIR");
 if(error)*error=[NSError errorWithDomain:NSCocoaErrorDomain code:NSFeatureUnsupportedError
   userInfo:@{NSLocalizedDescriptionKey:@"Binary archives are not supported by the legacy TGL adapter on this macOS. Compile the original shader library."}];
 return nil;
}
static BOOL noComputeICB(id self,SEL cmd){(void)self;(void)cmd;return NO;}
static BOOL getterHasType(id object,const char *name,const char *type){
 Method m=class_getInstanceMethod(object_getClass(object),sel_registerName(name));
 if(!m||method_getNumberOfArguments(m)!=2)return NO;
 char *actual=method_copyReturnType(m);BOOL ok=actual&&!strcmp(actual,type);free(actual);return ok;
}
static id bufferDescriptor(id self,SEL cmd,id descriptor){
 lastEntryError=NULL;
 if(!descriptor||!getterHasType(descriptor,"length","Q")||
    !getterHasType(descriptor,"resourceOptions","Q")||
    !getterHasType(descriptor,"pinnedGPUAddress","Q")||
    !getterHasType(descriptor,"contents","^v")||
    !getterHasType(descriptor,"deallocator","@?")){
  unsupported(cmd,"unsupported buffer descriptor getter contract");return nil;
 }
 uint64_t length=((uint64_t(*)(id,SEL))objc_msgSend)(descriptor,sel_registerName("length"));
 uint64_t options=((uint64_t(*)(id,SEL))objc_msgSend)(descriptor,sel_registerName("resourceOptions"));
 uint64_t address=((uint64_t(*)(id,SEL))objc_msgSend)(descriptor,sel_registerName("pinnedGPUAddress"));
 id deallocator=((id(*)(id,SEL))objc_msgSend)(descriptor,sel_registerName("deallocator"));
 const void *contents=((void*(*)(id,SEL))objc_msgSend)(descriptor,sel_registerName("contents"));
 // Only the recovered public option fields; never reinterpret new private
 // bits using the old allocator. Memoryless buffers are not supported here.
 if((options&~UINT64_C(0x3ff))||(options&15)>1||((options>>4)&15)>2||((options>>8)&3)>2){
  unsupported(cmd,"unsupported buffer resource option bits or modes");return nil;
 }
 if(address||deallocator){unsupported(cmd,"pinned GPU address or descriptor deallocator requires an unrecovered allocation contract");return nil;}
 // Do not let native page-rounding wrap. Native allocation retains its own
 // size/options/device-limit checks; this bridge does not create GPU addresses.
 uint64_t page=(uint64_t)getpagesize();
 if(!length||length>UINT64_MAX-(page-1)){unsupported(cmd,"invalid or page-rounding-overflow buffer length");return nil;}
 if(contents)return ((id(*)(id,SEL,const void*,NSUInteger,NSUInteger))legacyBuffers[1])(self,sel_registerName("newBufferWithBytes:length:options:"),contents,length,options);
 return ((id(*)(id,SEL,NSUInteger,NSUInteger))legacyBuffers[0])(self,sel_registerName("newBufferWithLength:options:"),length,options);
}

typedef struct {unsigned target;const char *selector;const char *types;IMP impl;} Entry;
static const Entry entries[]={
 {0,"getGlobalMemoryBuffer","@16@0:8",(IMP)rejectGlobal},
 {0,"getGlobalMemoryBuffer:","@24@0:8^{?=IIIIIIIIIIIIIIIIIIIIIIIIIIII[4I]}16",(IMP)rejectGlobalHeaderArgument},
 {0,"getGlobalMemoryBufferHeader","^{?=IIIIIIIIIIIIIIIIIIIIIIIIIIII[4I]}16@0:8",(IMP)rejectGlobalHeader},
 {0,"initializeComputeIndirectCommandBuffers","v16@0:8",(IMP)rejectVoid},
 {0,"newBufferWithDescriptor:","@24@0:8@16",(IMP)bufferDescriptor},
 {1,"generateIndirectionIndex:resourceIndex:","v32@0:8@16Q24",(IMP)rejectIndirection},
 {2,"initWithTexture:pixelFormat:resourceIndex:","@40@0:8@16Q24Q32",(IMP)initFormat},
 {2,"initWithTexture:pixelFormat:textureType:levels:slices:resourceIndex:","@80@0:8@16Q24Q32{_NSRange=QQ}40{_NSRange=QQ}56Q72",(IMP)initRange},
 {2,"initWithTexture:pixelFormat:textureType:levels:slices:swizzle:resourceIndex:","@84@0:8@16Q24Q32{_NSRange=QQ}40{_NSRange=QQ}56{?=CCCC}72Q76",(IMP)initSwizzle},
 {2,"newTextureViewWithPixelFormat:resourceIndex:","@32@0:8Q16Q24",(IMP)viewFormat},
 {2,"newTextureViewWithPixelFormat:textureType:levels:slices:resourceIndex:","@72@0:8Q16Q24{_NSRange=QQ}32{_NSRange=QQ}48Q64",(IMP)viewRange},
 {2,"newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:resourceIndex:","@76@0:8Q16Q24{_NSRange=QQ}32{_NSRange=QQ}48{?=CCCC}64Q68",(IMP)viewSwizzle},
};
// Full preflight before any change, invoked before device allocation. Native
// implementation or unexpected selector coverage wins: never overwrite it.
static BOOL installEntries(Class device,Class buffer,Class texture,uint64_t sentinel){
 if(installedDevice)return installedDevice==device&&installedBuffer==buffer&&installedTexture==texture;
 if(!device||!buffer||!texture||sentinel!=0)return NO;
 Class targets[]={device,buffer,texture};
 const char *legacy[]={"initWithTexture:pixelFormat:","initWithTexture:pixelFormat:textureType:levels:slices:","initWithTexture:pixelFormat:textureType:levels:slices:swizzle:","newTextureViewWithPixelFormat:","newTextureViewWithPixelFormat:textureType:levels:slices:","newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:"};
 const char *types[]={"@32@0:8@16Q24","@72@0:8@16Q24Q32{_NSRange=QQ}40{_NSRange=QQ}56","@76@0:8@16Q24Q32{_NSRange=QQ}40{_NSRange=QQ}56{?=CCCC}72","@24@0:8Q16","@64@0:8Q16Q24{_NSRange=QQ}32{_NSRange=QQ}48","@68@0:8Q16Q24{_NSRange=QQ}32{_NSRange=QQ}48{?=CCCC}64"};
 for(unsigned i=0;i<6;i++){
  Method m=class_getInstanceMethod(texture,sel_registerName(legacy[i]));
  if(!m||strcmp(method_getTypeEncoding(m),types[i]))return NO;
  legacyViews[i]=method_getImplementation(m);
 }
 const char *bufferNames[]={"newBufferWithLength:options:","newBufferWithBytes:length:options:"};
 const char *bufferTypes[]={"@32@0:8Q16Q24","@40@0:8r^v16Q24Q32"};
 for(unsigned i=0;i<2;i++){
  Method m=class_getInstanceMethod(device,sel_registerName(bufferNames[i]));
  if(!m||strcmp(method_getTypeEncoding(m),bufferTypes[i]))return NO;
  legacyBuffers[i]=method_getImplementation(m);
 }
 for(unsigned i=0;i<12;i++)if(class_getInstanceMethod(targets[entries[i].target],sel_registerName(entries[i].selector)))return NO;
 Method archive=class_getInstanceMethod(device,sel_registerName("newBinaryArchiveWithDescriptor:error:"));
 if(!archive||strcmp(method_getTypeEncoding(archive),"@32@0:8@16^@24"))return NO;
 invalidIndex=sentinel;
 for(unsigned i=0;i<12;i++)if(!class_addMethod(targets[entries[i].target],sel_registerName(entries[i].selector),entries[i].impl,entries[i].types))return NO;
 // Global-memory consumers include mesh emulation. TGL has no recovered
 // implementation; do not inherit a newer host profile's optimistic values.
 // TGL implements the fixed-stride encoder only. Tahoe's inherited YES
 // routes validation through the base dynamic-stride stub, which throws.
 const char *caps[]={"supportsDynamicAttributeStride","supportsCMPIndirectCommandBuffers","supportsMeshShaders","supportsMeshShadersInICB","supportsBinaryArchives","supportsAIRNTBinaryArchiveSpecializedFunctions","supportsAIRNTBinaryArchiveFunctionPointers","supportsAIRNTBinaryArchiveStitchedFunctions"};
 for(unsigned i=0;i<sizeof(caps)/sizeof(caps[0]);i++)class_replaceMethod(device,sel_registerName(caps[i]),(IMP)noComputeICB,"c16@0:8");
 class_replaceMethod(device,sel_registerName("newBinaryArchiveWithDescriptor:error:"),(IMP)rejectBinaryArchive,"@32@0:8@16^@24");
 installedDevice=device;installedBuffer=buffer;installedTexture=texture;
 return YES;
}
