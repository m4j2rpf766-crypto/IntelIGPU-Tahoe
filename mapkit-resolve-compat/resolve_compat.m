// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <os/log.h>
#import "../app-blank-candidate/texture_sync_compat.m"
static id (*originalRender)(id,SEL,MTLRenderPassDescriptor*);
static void (*originalEnd)(id,SEL);
static char resolveKey;
static id (*originalNewTexture)(id,SEL,MTLTextureDescriptor*);
static id compatibleNewTexture(id device,SEL selector,MTLTextureDescriptor *d){
 if((d.pixelFormat==MTLPixelFormatBGRA8Unorm_sRGB||d.pixelFormat==MTLPixelFormatBGRA8Unorm)&&
    ((d.textureType==MTLTextureType2DMultisample&&d.sampleCount==4)||(d.textureType==MTLTextureType2D&&d.sampleCount==1))&&d.usage!=MTLTextureUsageUnknown){
  MTLTextureDescriptor *copy=[d copy];copy.usage|=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;return originalNewTexture(device,selector,copy);
 }
 return originalNewTexture(device,selector,d);
}
static id<MTLRenderPipelineState> resolvePipeline(id<MTLDevice> device, MTLPixelFormat format) {
 static NSMutableDictionary *cache;
 @synchronized([NSObject class]) {
  if(!cache) cache=[NSMutableDictionary new];
  NSString *key=[NSString stringWithFormat:@"%p-%lu",device,(unsigned long)format];
  id<MTLRenderPipelineState> pipeline=cache[key]; if(pipeline)return pipeline;
  NSString *source=@"#include <metal_stdlib>\nusing namespace metal; struct V { float4 p [[position]]; }; vertex V rv(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return {float4(p[i],0,1)}; } fragment float4 rf(V v [[stage_in]],texture2d_ms<float,access::read> t [[texture(0)]]) { float4 c=0; for(uint i=0;i<t.get_num_samples();i++) c+=t.read(uint2(v.p.xy),i); return c/float(t.get_num_samples()); }";
  NSError *error=nil; id<MTLLibrary> library=[device newLibraryWithSource:source options:nil error:&error];
  if(!library){NSLog(@"resolve compile error %@",error);return nil;}
  MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[library newFunctionWithName:@"rv"];d.fragmentFunction=[library newFunctionWithName:@"rf"];d.colorAttachments[0].pixelFormat=format;
  pipeline=[device newRenderPipelineStateWithDescriptor:d error:&error];
  if(pipeline)cache[key]=pipeline;else NSLog(@"resolve pipeline error %@",error);
  return pipeline;
 }
}
static id compatibleRender(id<MTLCommandBuffer> cb,SEL selector,MTLRenderPassDescriptor *descriptor){
 MTLRenderPassColorAttachmentDescriptor *a=descriptor.colorAttachments[0];
 BOOL supported=a.texture.textureType==MTLTextureType2DMultisample&&a.texture.sampleCount==4&&a.resolveTexture.textureType==MTLTextureType2D&&a.texture.pixelFormat==a.resolveTexture.pixelFormat&&(a.texture.pixelFormat==MTLPixelFormatBGRA8Unorm_sRGB||a.texture.pixelFormat==MTLPixelFormatBGRA8Unorm)&&a.level==0&&a.slice==0&&a.depthPlane==0&&a.resolveLevel==0&&a.resolveSlice==0&&a.resolveDepthPlane==0&&(a.storeAction==MTLStoreActionMultisampleResolve||a.storeAction==MTLStoreActionStoreAndMultisampleResolve);
 for(NSUInteger i=1;i<8;i++)if(descriptor.colorAttachments[i].texture)supported=NO;
 if(a.texture.width!=a.resolveTexture.width||a.texture.height!=a.resolveTexture.height||
 (a.texture.usage!=MTLTextureUsageUnknown&&!(a.texture.usage&MTLTextureUsageShaderRead))||
 (a.resolveTexture.usage!=MTLTextureUsageUnknown&&!(a.resolveTexture.usage&MTLTextureUsageRenderTarget))||
 (descriptor.renderTargetWidth&&descriptor.renderTargetWidth!=a.texture.width)||
 (descriptor.renderTargetHeight&&descriptor.renderTargetHeight!=a.texture.height)||descriptor.renderTargetArrayLength>1)supported=NO;
 if(!supported)return originalRender(cb,selector,descriptor);
 os_log(OS_LOG_DEFAULT,"ReimsMapResolve sourceUsage=%lu destinationUsage=%lu samples=%lu",(unsigned long)a.texture.usage,(unsigned long)a.resolveTexture.usage,(unsigned long)a.texture.sampleCount);
 id<MTLRenderPipelineState> pipeline=resolvePipeline(cb.device,a.resolveTexture.pixelFormat);
 if(!pipeline)return originalRender(cb,selector,descriptor);
 MTLRenderPassDescriptor *copy=[descriptor copy];copy.colorAttachments[0].storeAction=MTLStoreActionStore;copy.colorAttachments[0].resolveTexture=nil;
 id encoder=originalRender(cb,selector,copy);
 if(encoder)objc_setAssociatedObject(encoder,&resolveKey,@[cb,a.texture,a.resolveTexture,pipeline],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 return encoder;
}
static void compatibleEnd(id encoder,SEL selector){
 NSArray *state=objc_getAssociatedObject(encoder,&resolveKey);
 objc_setAssociatedObject(encoder,&resolveKey,nil,OBJC_ASSOCIATION_ASSIGN);
 originalEnd(encoder,selector);if(!state)return;
 id<MTLCommandBuffer> cb=state[0];id<MTLTexture> src=state[1],dst=state[2];
 MTLRenderPassDescriptor *p=[MTLRenderPassDescriptor renderPassDescriptor];p.colorAttachments[0].texture=dst;p.colorAttachments[0].loadAction=MTLLoadActionDontCare;p.colorAttachments[0].storeAction=MTLStoreActionStore;
 id<MTLRenderCommandEncoder> out=originalRender(cb,@selector(renderCommandEncoderWithDescriptor:),p);
 if(!out)abort();
 [out setRenderPipelineState:state[3]];[out setFragmentTexture:src atIndex:0];[out drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];originalEnd(out,@selector(endEncoding));
}
BOOL ReimsInstallMapResolveCompat(void){
 static BOOL installed;if(installed)return YES;
 Class cb=objc_getClass("MTLIGAccelCommandBuffer"),render=objc_getClass("MTLIGAccelRenderCommandEncoder"),device=objc_getClass("MTLIGAccelDevice");
 Method r=class_getInstanceMethod(cb,@selector(renderCommandEncoderWithDescriptor:)),e=class_getInstanceMethod(render,@selector(endEncoding)),n=class_getInstanceMethod(device,@selector(newTextureWithDescriptor:));
 Dl_info ri={0},ei={0},ni={0};
 if(!r||!e||!n||!dladdr((void*)method_getImplementation(r),&ri)||!dladdr((void*)method_getImplementation(e),&ei)||!dladdr((void*)method_getImplementation(n),&ni)||ri.dli_fbase!=ei.dli_fbase||ri.dli_fbase!=ni.dli_fbase||
 (uintptr_t)method_getImplementation(r)-(uintptr_t)ri.dli_fbase!=0x7f496||
 (uintptr_t)method_getImplementation(e)-(uintptr_t)ri.dli_fbase!=0x4a8cf)return NO;
 if(!reimsInstallTextureSync(ri.dli_fbase))return NO;
 originalRender=(void*)method_getImplementation(r);originalEnd=(void*)method_getImplementation(e);originalNewTexture=(void*)method_getImplementation(n);
 method_setImplementation(n,(IMP)compatibleNewTexture);method_setImplementation(r,(IMP)compatibleRender);method_setImplementation(e,(IMP)compatibleEnd);
 installed=YES;return YES;
}
#ifdef REIMS_MAP_RESOLVE_STANDALONE
__attribute__((constructor)) static void install(void){if(!ReimsInstallMapResolveCompat())abort();}
#endif
