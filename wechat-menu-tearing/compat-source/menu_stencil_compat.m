// MRC. Pinned TGL/Tahoe adapter for transient stencil-only menu passes.
// Stencil load=Clear/store=DontCare has no defined contents outside the pass.
// Use a real supported combined depth/stencil backing, preserving 8-bit stencil.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <os/log.h>
static char menuAlternatePipelineKey,menuAlternateDepthKey,menuEncoderKey;
static id(*menuNewPipeline)(id,SEL,MTLRenderPipelineDescriptor*,NSError**);
static id(*menuNewPipelineOptions)(id,SEL,MTLRenderPipelineDescriptor*,MTLPipelineOption,MTLRenderPipelineReflection**,NSError**);
static id(*menuNewDepth)(id,SEL,MTLDepthStencilDescriptor*);
static id(*menuNewEncoder)(id,SEL,MTLRenderPassDescriptor*);
static void(*menuSetPipeline)(id,SEL,id);
static void(*menuSetDepth)(id,SEL,id);
static _Thread_local unsigned menuPipelineDepth;
static BOOL menuOnlyStencilPipeline(MTLRenderPipelineDescriptor*p){return p.depthAttachmentPixelFormat==MTLPixelFormatInvalid&&p.stencilAttachmentPixelFormat==MTLPixelFormatStencil8&&p.rasterSampleCount==1;}
static BOOL menuPairPipeline(id device,id original,MTLRenderPipelineDescriptor*p,NSError**error){
 if(!original||!menuOnlyStencilPipeline(p))return YES;
 MTLRenderPipelineDescriptor*copy=[p copy];copy.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;copy.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
 id alternate=menuNewPipelineOptions(device,@selector(newRenderPipelineStateWithDescriptor:options:reflection:error:),copy,0,NULL,error);[copy release];
 if(!alternate)return NO;
 objc_setAssociatedObject(original,&menuAlternatePipelineKey,alternate,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[alternate release];return YES;
}
static id menuPipeline(id d,SEL s,MTLRenderPipelineDescriptor*p,NSError**e){
 if(menuPipelineDepth)return menuNewPipeline(d,s,p,e);menuPipelineDepth++;
 id result=menuNewPipeline(d,s,p,e);if(!menuPairPipeline(d,result,p,e)){[result release];result=nil;}menuPipelineDepth--;return result;
}
static id menuPipelineOptions(id d,SEL s,MTLRenderPipelineDescriptor*p,MTLPipelineOption o,MTLRenderPipelineReflection**r,NSError**e){
 if(menuPipelineDepth)return menuNewPipelineOptions(d,s,p,o,r,e);menuPipelineDepth++;
 id result=menuNewPipelineOptions(d,s,p,o,r,e);if(!menuPairPipeline(d,result,p,e)){[result release];result=nil;if(r)*r=nil;}menuPipelineDepth--;return result;
}
static id menuDepth(id d,SEL s,MTLDepthStencilDescriptor*p){
 id result=menuNewDepth(d,s,p);if(!result)return nil;
 // With no depth attachment the caller's depth test/writes are disabled.
 // Keep that behavior after adding the real combined backing.
 if(p.depthCompareFunction!=MTLCompareFunctionAlways||p.depthWriteEnabled){
  MTLDepthStencilDescriptor*copy=[p copy];copy.depthCompareFunction=MTLCompareFunctionAlways;copy.depthWriteEnabled=NO;
  id alternate=menuNewDepth(d,s,copy);[copy release];if(!alternate){[result release];return nil;}
  objc_setAssociatedObject(result,&menuAlternateDepthKey,alternate,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[alternate release];
 }
 return result;
}
static BOOL menuTransientStencil(MTLRenderPassDescriptor*p){
 id<MTLTexture>t=p.stencilAttachment.texture;
 return !p.depthAttachment.texture&&t&&t.pixelFormat==MTLPixelFormatStencil8&&t.textureType==MTLTextureType2D&&t.storageMode==MTLStorageModePrivate&&t.usage==MTLTextureUsageRenderTarget&&t.sampleCount==1&&t.mipmapLevelCount==1&&t.arrayLength==1&&p.stencilAttachment.level==0&&p.stencilAttachment.slice==0&&p.stencilAttachment.depthPlane==0&&p.stencilAttachment.loadAction==MTLLoadActionClear&&p.stencilAttachment.storeAction==MTLStoreActionDontCare&&!p.stencilAttachment.resolveTexture;
}
static id menuEncoder(id cb,SEL s,MTLRenderPassDescriptor*p){
 if(!menuTransientStencil(p))return menuNewEncoder(cb,s,p);
 id<MTLTexture>original=p.stencilAttachment.texture;
 MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:original.width height:original.height mipmapped:NO];td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;td.hazardTrackingMode=MTLHazardTrackingModeTracked;
 id<MTLTexture>backing=[[(id<MTLCommandBuffer>)cb device] newTextureWithDescriptor:td];if(!backing)return nil;
 MTLRenderPassDescriptor*copy=[p copy];copy.stencilAttachment.texture=backing;copy.depthAttachment.texture=backing;copy.depthAttachment.level=0;copy.depthAttachment.slice=0;copy.depthAttachment.depthPlane=0;copy.depthAttachment.loadAction=MTLLoadActionClear;copy.depthAttachment.storeAction=MTLStoreActionDontCare;copy.depthAttachment.clearDepth=1;
 id encoder=menuNewEncoder(cb,s,copy);[copy release];
 if(encoder){objc_setAssociatedObject(encoder,&menuEncoderKey,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  // Also supports unretained-reference command buffers: release only after
  // actual GPU completion. No synthetic completion or discarded dependencies.
  [(id<MTLCommandBuffer>)cb addCompletedHandler:^(id<MTLCommandBuffer>done){(void)done;(void)backing.width;}];
 }
 [backing release];return encoder;
}
static void menuPipelineBind(id e,SEL s,id p){
 if(p&&objc_getAssociatedObject(e,&menuEncoderKey)){id alternate=objc_getAssociatedObject(p,&menuAlternatePipelineKey);if(!alternate)[NSException raise:@"ReimsMenuStencilPipeline" format:@"Missing compatible pipeline for transient stencil-only pass"];p=alternate;}
 menuSetPipeline(e,s,p);
}
static void menuDepthBind(id e,SEL s,id d){if(d&&objc_getAssociatedObject(e,&menuEncoderKey)){id alternate=objc_getAssociatedObject(d,&menuAlternateDepthKey);if(alternate)d=alternate;}menuSetDepth(e,s,d);}
static BOOL reimsInstallMenuStencilCompat(const void*nativeImage){
 if(strcmp(getprogname(),"DockHelper"))return YES;
 static BOOL installed;if(installed)return YES;
 Class device=objc_getClass("MTLIGAccelDevice"),cb=objc_getClass("MTLIGAccelCommandBuffer"),enc=objc_getClass("MTLIGAccelRenderCommandEncoder");
 Class classes[]={device,device,device,cb,enc,enc};
 const char*names[]={"newRenderPipelineStateWithDescriptor:error:","newRenderPipelineStateWithDescriptor:options:reflection:error:","newDepthStencilStateWithDescriptor:","renderCommandEncoderWithDescriptor:","setRenderPipelineState:","setDepthStencilState:"};
 const unsigned counts[]={4,6,3,3,3,3};Method methods[6];IMP original[6];
 for(unsigned i=0;i<6;i++){methods[i]=class_getInstanceMethod(classes[i],sel_registerName(names[i]));Dl_info info={0};if(!methods[i]||method_getNumberOfArguments(methods[i])!=counts[i])return NO;original[i]=method_getImplementation(methods[i]);if(!dladdr((void*)original[i],&info))return NO;
  if(i<2){uintptr_t offsets[]={0x1d189,0x11953d};if(!info.dli_fname||!strstr(info.dli_fname,"/Metal.framework/")||(uintptr_t)original[i]-(uintptr_t)info.dli_fbase!=offsets[i])return NO;}else if(info.dli_fbase!=nativeImage)return NO;}
 menuNewPipeline=(void*)original[0];menuNewPipelineOptions=(void*)original[1];menuNewDepth=(void*)original[2];menuNewEncoder=(void*)original[3];menuSetPipeline=(void*)original[4];menuSetDepth=(void*)original[5];IMP replacement[]={(IMP)menuPipeline,(IMP)menuPipelineOptions,(IMP)menuDepth,(IMP)menuEncoder,(IMP)menuPipelineBind,(IMP)menuDepthBind};
 for(unsigned i=0;i<6;i++){class_addMethod(classes[i],sel_registerName(names[i]),original[i],method_getTypeEncoding(methods[i]));class_replaceMethod(classes[i],sel_registerName(names[i]),replacement[i],method_getTypeEncoding(methods[i]));}
 installed=YES;return YES;
}
