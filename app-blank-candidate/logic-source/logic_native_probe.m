#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#import <objc/message.h>
static id<MTLBuffer> makeVertices(id<MTLDevice>d,const void*p,NSUInteger n){
 if(getenv("ANGLE_NATIVE_BUFFER_LENGTH")){id<MTLBuffer>b=[d newBufferWithLength:n options:MTLResourceStorageModeShared];memcpy(b.contents,p,n);return b;}
 return [d newBufferWithBytes:p length:n options:MTLResourceStorageModeShared];
}
int main(void){alarm(25);@autoreleasepool{
 id<MTLDevice>d=MTLCreateSystemDefaultDevice();NSError*err=nil;
 if(![d.name containsString:@"TGL"])return 2;
 NSString*vs=[NSString stringWithContentsOfFile:@"wechat-probe-shader-1.metal" encoding:NSUTF8StringEncoding error:&err];
 NSString*fs=[NSString stringWithContentsOfFile:@"wechat-probe-shader-2.metal" encoding:NSUTF8StringEncoding error:&err];
 const char*trace=getenv("WECHAT_PROBE_TRACE");if(trace&&!dlopen(trace,RTLD_NOW|RTLD_LOCAL)){puts(dlerror());return 8;}
 NSString*simple=@"#include <metal_stdlib>\nusing namespace metal;struct V{float2 p [[attribute(0)]];};vertex float4 vs(V v [[stage_in]]){return float4(v.p,0.5,1);}fragment float4 fs(){return float4(0,1,0,1);}";
 id<MTLLibrary>sl=[d newLibraryWithSource:simple options:nil error:&err];
 id<MTLLibrary>vl=[d newLibraryWithSource:vs options:nil error:&err],fl=[d newLibraryWithSource:fs options:nil error:&err];
 if(!sl||!vl||!fl){NSLog(@"library %@",err);return 3;}
 MTLFunctionConstantValues*cv=[MTLFunctionConstantValues new];bool no=false;
 for(unsigned i=0;i<6;i++)[cv setConstantValue:&no type:MTLDataTypeBool atIndex:i];
 if(getenv("ANGLE_NATIVE_CONSTANTS")){bool yes=true;[cv setConstantValue:&yes type:MTLDataTypeBool atIndex:0];[cv setConstantValue:&yes type:MTLDataTypeBool atIndex:4];}
 MTLFunctionConstantValues*fv=cv;
 if(getenv("ANGLE_NATIVE_NAMED")){cv=[MTLFunctionConstantValues new];fv=[MTLFunctionConstantValues new];bool yes=true;[cv setConstantValue:&no type:MTLDataTypeBool withName:@"ANGLERasterizerDisabled"];[fv setConstantValue:&no type:MTLDataTypeBool withName:@"ANGLEMultisampledRendering"];[fv setConstantValue:&no type:MTLDataTypeBool withName:@"ANGLEDepthWriteEnabled"];for(MTLFunctionConstantValues*x in @[cv,fv]){[x setConstantValue:&yes type:MTLDataTypeBool withName:@"ANGLEUseSampleCompareGradient"];[x setConstantValue:&yes type:MTLDataTypeBool withName:@"ANGLEEmulateAlphaToCoverage"];[x setConstantValue:&no type:MTLDataTypeBool withName:@"ANGLEWriteHelperSampleMask"];}}
 id<MTLFunction>vf=[vl newFunctionWithName:@"main0" constantValues:cv error:&err],ff=[fl newFunctionWithName:@"main0" constantValues:fv error:&err];
 if(!vf||!ff){NSLog(@"function %@",err);return 4;}
 const float verts[]={-1,-1,3,-1,-1,3};
 uint32_t uniform[16]={0,0,0,0x3f800000,0x00400040,0x817f7f7f,0,0x00100000,0,0,0,0,0,0xffffffff,0,0};
 id<MTLBuffer>v=makeVertices(d,verts,sizeof(verts));
 id<MTLCommandQueue>q=[d newCommandQueue];
 unsigned start=getenv("ANGLE_NATIVE_MODE")?(unsigned)atoi(getenv("ANGLE_NATIVE_MODE")):0;
 unsigned end=getenv("ANGLE_NATIVE_MODE")?start+1:32;
 for(unsigned mode=start;mode<end;mode++){
 MTLRenderPipelineDescriptor*p=[MTLRenderPipelineDescriptor new];p.vertexFunction=(mode&1)?vf:[sl newFunctionWithName:@"vs"];p.fragmentFunction=(mode&2)?ff:[sl newFunctionWithName:@"fs"];
 p.vertexDescriptor.attributes[0].format=MTLVertexFormatFloat2;p.vertexDescriptor.attributes[0].bufferIndex=0;p.vertexDescriptor.layouts[0].stride=8;p.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 if(getenv("TEST_LOGIC_OP")){((void(*)(id,SEL,NSUInteger))objc_msgSend)(p,sel_registerName("setLogicOperation:"),atoi(getenv("TEST_LOGIC_OP")));((void(*)(id,SEL,BOOL))objc_msgSend)(p,sel_registerName("setLogicOperationEnabled:"),getenv("TEST_LOGIC_ENABLED")!=NULL);}
 [p.description writeToFile:@"/tmp/native-pipeline-description.txt" atomically:YES encoding:NSUTF8StringEncoding error:nil];
 id<MTLRenderPipelineState>ps=getenv("ANGLE_NATIVE_DEFER_PIPELINE")?nil:[d newRenderPipelineStateWithDescriptor:p error:&err];if(!ps&&!getenv("ANGLE_NATIVE_DEFER_PIPELINE")){NSLog(@"pipeline %@",err);return 5;}
 MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];td.storageMode=(mode&4)?MTLStorageModeManaged:MTLStorageModePrivate;td.usage=getenv("ANGLE_NATIVE_USAGE")?7:MTLTextureUsageRenderTarget;
 id<MTLTexture>t=[d newTextureWithDescriptor:td];id<MTLBuffer>b=[d newBufferWithLength:16384 options:MTLResourceStorageModeShared];memset(b.contents,0xa5,16384);
 id<MTLCommandBuffer>cb=getenv("ANGLE_NATIVE_UNRETAINED")?[q commandBufferWithUnretainedReferences]:[q commandBuffer];MTLRenderPassDescriptor*rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=t;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.colorAttachments[0].clearColor=MTLClearColorMake(1,0,0,1);
 if(getenv("ANGLE_NATIVE_EMPTY_ACTIONS")){for(unsigned i=1;i<8;i++)rp.colorAttachments[i].storeAction=MTLStoreActionDontCare;rp.depthAttachment.loadAction=MTLLoadActionDontCare;rp.stencilAttachment.loadAction=MTLLoadActionDontCare;}
 if(mode&16){id<MTLRenderCommandEncoder>clear=[cb renderCommandEncoderWithDescriptor:rp];[clear endEncoding];[cb commit];[cb waitUntilCompleted];cb=getenv("ANGLE_NATIVE_UNRETAINED")?[q commandBufferWithUnretainedReferences]:[q commandBuffer];rp.colorAttachments[0].loadAction=MTLLoadActionLoad;rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);}
 if(getenv("ANGLE_NATIVE_READ_CLEAR")){id<MTLCommandBuffer>sync=[q commandBuffer];id<MTLBlitCommandEncoder>be=[sync blitCommandEncoder];if(getenv("ANGLE_NATIVE_SYNC_RESOURCE"))[be synchronizeResource:t];else [be synchronizeTexture:t slice:0 level:0];[be endEncoding];[sync commit];[sync waitUntilCompleted];unsigned char rows[16384];for(unsigned y=0;y<64;y++)[t getBytes:rows+y*256 bytesPerRow:256 fromRegion:MTLRegionMake2D(0,y,64,1) mipmapLevel:0];printf("read_clear=%u,%u,%u,%u\n",rows[0],rows[1],rows[2],rows[3]);}
 if(!ps){ps=[d newRenderPipelineStateWithDescriptor:p error:&err];if(!ps)return 5;[v release];v=makeVertices(d,verts,sizeof(verts));}
 [rp.description writeToFile:@"/tmp/native-render-description.txt" atomically:YES encoding:NSUTF8StringEncoding error:nil];[t.description writeToFile:@"/tmp/native-texture-description.txt" atomically:YES encoding:NSUTF8StringEncoding error:nil];
 id<MTLRenderCommandEncoder>e=[cb renderCommandEncoderWithDescriptor:rp];if(getenv("ANGLE_NATIVE_STENCIL_REF"))[e setStencilReferenceValue:0];if(!getenv("ANGLE_NATIVE_LATE_PIPELINE"))[e setRenderPipelineState:ps];
 if(getenv("ANGLE_NATIVE_DEPTH")){MTLDepthStencilDescriptor*dd=[MTLDepthStencilDescriptor new];dd.depthCompareFunction=MTLCompareFunctionAlways;dd.depthWriteEnabled=NO;if(getenv("ANGLE_NATIVE_STENCIL")){dd.frontFaceStencil=[MTLStencilDescriptor new];dd.backFaceStencil=[MTLStencilDescriptor new];}dd.frontFaceStencil.stencilCompareFunction=MTLCompareFunctionAlways;dd.backFaceStencil.stencilCompareFunction=MTLCompareFunctionAlways;if(getenv("ANGLE_NATIVE_STENCIL_MASK8")){dd.frontFaceStencil.readMask=0xff;dd.frontFaceStencil.writeMask=0xff;dd.backFaceStencil.readMask=0xff;dd.backFaceStencil.writeMask=0xff;}id<MTLDepthStencilState>ds=[d newDepthStencilStateWithDescriptor:dd];[e setDepthStencilState:ds];[ds release];[dd release];}
 if(getenv("ANGLE_NATIVE_VERTEX_BYTES"))[e setVertexBytes:verts length:sizeof(verts) atIndex:0];else [e setVertexBuffer:v offset:0 atIndex:0];
 if(getenv("ANGLE_NATIVE_DEFAULT_ATTRS")){float defaults[64]={0};for(unsigned i=0;i<16;i++)defaults[4*i+3]=1;[e setVertexBytes:defaults length:sizeof(defaults) atIndex:16];}
 [e setFragmentBytes:uniform length:sizeof(uniform) atIndex:17];[e setVertexBytes:uniform length:sizeof(uniform) atIndex:17];
 if(getenv("ANGLE_NATIVE_VIEW")){[e setViewport:(MTLViewport){0,0,64,64,0,1}];[e setScissorRect:(MTLScissorRect){0,0,64,64}];}
 if(getenv("ANGLE_NATIVE_LATE_PIPELINE"))[e setRenderPipelineState:ps];
 [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];
 id<MTLBlitCommandEncoder>bl=[cb blitCommandEncoder];if((mode&12)==12){if(getenv("ANGLE_NATIVE_SYNC_RESOURCE"))[bl synchronizeResource:t];else [bl synchronizeTexture:t slice:0 level:0];}[bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:16384];[bl endEncoding];[cb commit];[cb waitUntilCompleted];
 unsigned bad=0;unsigned char*x=b.contents;for(unsigned i=0;i<4096;i++)if(x[4*i]!=0||x[4*i+1]!=255||x[4*i+2]!=0||x[4*i+3]!=255)bad++;
 unsigned zero=0;for(unsigned i=0;i<4096;i++)zero+=!x[4*i]&&!x[4*i+1]&&!x[4*i+2]&&!x[4*i+3];printf("zero=%u ",zero);
 printf("mode=%u status=%lu bad=%u first=%u,%u,%u,%u error=%s\n",mode,(unsigned long)cb.status,bad,x[0],x[1],x[2],x[3],cb.error.description.UTF8String?:"");fflush(stdout);
 [b release];[t release];[ps release];[p release];
 }
 return 0;
}}
