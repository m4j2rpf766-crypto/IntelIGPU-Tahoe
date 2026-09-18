#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include "gen12_scratch.h"
#include "hevc_recovery.h"
#include <IOSurface/IOSurface.h>
static uint32_t surface_ids[1024];
static int production_mode;
static char production_va[2048];
void reims_configure_production(const char*path){snprintf(production_va,sizeof(production_va),"%s",path);production_mode=1;}
void reims_dump_surfaces(const char*dir){
 if(!dir||!getenv("REIMS_HEVC_DUMP_SURFACES"))return;
 static atomic_uint dump_frame;
 unsigned frame=atomic_fetch_add(&dump_frame,1)+1;
 const char*selected=getenv("REIMS_HEVC_DUMP_FRAME");
 if(selected&&frame!=(unsigned)strtoul(selected,NULL,10))return;
 fprintf(stderr,"HEVC_SURFACE_FRAME frame=%u\n",frame);
 for(unsigned k=0;k<1024;k++)if(surface_ids[k]){
  IOSurfaceRef s=IOSurfaceLookup(surface_ids[k]);if(!s)continue;
  kern_return_t lock=IOSurfaceLock(s,kIOSurfaceLockReadOnly,NULL);
  fprintf(stderr,"HEVC_SURFACE handle=%u id=%u lock=%x width=%zu height=%zu pitch=%zu format=%x alloc=%zu\n",k,surface_ids[k],lock,IOSurfaceGetWidth(s),IOSurfaceGetHeight(s),IOSurfaceGetBytesPerRow(s),IOSurfaceGetPixelFormat(s),IOSurfaceGetAllocSize(s));
  for(unsigned p=0;p<IOSurfaceGetPlaneCount(s);p++)fprintf(stderr,"HEVC_PLANE handle=%u plane=%u offset=%zu pitch=%zu width=%zu height=%zu\n",k,p,(char*)IOSurfaceGetBaseAddressOfPlane(s,p)-(char*)IOSurfaceGetBaseAddress(s),IOSurfaceGetBytesPerRowOfPlane(s,p),IOSurfaceGetWidthOfPlane(s,p),IOSurfaceGetHeightOfPlane(s,p));
  if(!lock){char path[2048];snprintf(path,sizeof(path),"%s/surface-%u.bin",dir,k);FILE*f=fopen(path,"wb");if(f){fwrite(IOSurfaceGetBaseAddress(s),1,IOSurfaceGetAllocSize(s),f);fclose(f);}IOSurfaceUnlock(s,kIOSurfaceLockReadOnly,NULL);}
  CFRelease(s);
 }
}
extern int IOAccelVideoContextSubmitDataBuffers(void*,unsigned,unsigned*,unsigned*);
extern void* IOAccelResourceCreate(void*,const void*,size_t);
extern size_t IOAccelResourceGetDataSize(void*);
extern void* IOAccelResourceGetClientShared(void*,void**);
static atomic_uint sequence;
static atomic_uint resource_sequence;
static void *captured_shared;
extern int reims_prepare_gen12_probe(void*,const void*,size_t,const void*,size_t,const char*);
extern int reims_submit_gen12_probe(void*,const void*,size_t,const void*,size_t,const char*,void*,unsigned,unsigned*,unsigned*);
static atomic_uint execute_count;
static atomic_uint encode_completed;
static atomic_uint final_flush_count;
static int read_mem(uintptr_t a,void*d,size_t n){mach_vm_size_t got=0;return a&&mach_vm_read_overwrite(mach_task_self(),a,n,(mach_vm_address_t)d,&got)==0&&got==n;}
static void blob(unsigned n,const char*name,uintptr_t a,size_t bytes){if(!a||!bytes||bytes>1048576)return;void*b=malloc(bytes);if(!b)return;if(read_mem(a,b,bytes)){const char*dir=getenv("REIMS_HEVC_CAPTURE_DIR");if(dir){char path[2048];snprintf(path,sizeof(path),"%s/%u-%s.bin",dir,n,name);FILE*f=fopen(path,"wb");if(f){fwrite(b,1,bytes,f);fclose(f);}}}free(b);}
static int blocked_submit(void*io,unsigned flags,unsigned*out,unsigned*error){unsigned n=atomic_fetch_add(&sequence,1);uintptr_t c=(uintptr_t)out-0x54;uint64_t w[32]={0};int valid=out&&read_mem(c,w,sizeof(w))&&w[1]==(uintptr_t)io;
 int previous=reims_context_error(io);if(previous){if(out)*out=0;if(error)*error=(unsigned)previous;return previous;}
 void*shared=valid?(void*)w[0]:NULL;valid=valid&&shared;
 if(valid&&flags==0&&w[2]==0&&w[4]==0&&w[12]==0){int(*real)(void*,unsigned,unsigned*,unsigned*)=IOAccelVideoContextSubmitDataBuffers;if(real&&real!=blocked_submit){fprintf(stderr,"HEVC_CAPTURE_FORWARD_EMPTY_INIT submit=%u\n",n);return real(io,flags,out,error);}}
 if(valid&&captured_shared)fprintf(stderr,"HEVC_SHARED_NAMESPACE matches=%d\n",shared==captured_shared);
 fprintf(stderr,"HEVC_CAPTURE_BLOCKED submit=%u flags=%x valid_layout=%d\n",n,flags,valid);
 if(valid&&getenv("REIMS_HEVC_PROBE_SCRATCH")&&captured_shared){
  const size_t sizes[]={81920,55296,20480,12288,51200,55296};
  ReimsScratch scratch[6]={0};unsigned created=0;
  for(unsigned i=0;i<6;i++){
   if(!reims_scratch_create(captured_shared,sizes[i],&scratch[i]))break;
   created++;
   fprintf(stderr,"HEVC_SCRATCH_CREATED slot=%u requested=%zu allocated=%zu handle=%u\n",i,sizes[i],scratch[i].bytes,scratch[i].handle);
  }
  for(unsigned i=0;i<created;i++)reims_scratch_destroy(&scratch[i]);
  fprintf(stderr,"HEVC_SCRATCH_PROBE created=%u released=%u gpu_submitted=0\n",created,created);
 }
 if(valid){blob(n,"context",c,sizeof(w));if(w[4]>=w[2])blob(n,"commands",w[2],w[4]-w[2]);if(w[12]>=w[16])blob(n,"sideband",w[16],w[12]-w[16]);}
 if(valid&&w[4]-w[2]==12296&&w[12]-w[16]==4152){
  uint32_t cmd[3074],side[1038];
  uint32_t expected[]={0x13004082,0,0,0,0,0x05000000};
  int safe=read_mem(w[2],cmd,sizeof(cmd))&&read_mem(w[16],side,sizeof(side));
  if(safe){expected[3]=cmd[3];safe=(cmd[3]&63)==side[1035]&&!memcmp(cmd,expected,sizeof(expected));}
  for(unsigned j=6;safe&&j<3074;j++)if(cmd[j])safe=0;
  safe=safe&&side[1033]==0x38000&&side[1034]==6&&side[9]==0x04008200&&side[12]==0x31&&side[13]!=0&&side[14]==0;
  for(unsigned j=15;safe&&j<1033;j++)if(side[j])safe=0;
  if(safe&&(production_mode||getenv("REIMS_HEVC_EXECUTE_GEN12_STREAM")||atomic_fetch_add(&final_flush_count,1)==0)){
   fprintf(stderr,"HEVC_GEN12_FORWARD_NATIVE_FINAL_FLUSH event=%u\n",side[1035]);
   int completed=reims_execute_owned_batch(shared,io,flags,side[1035],out,error,NULL,0,side,1038);
   if(completed==2)return 0;
   if(completed<0)return error&&*error?(int)*error:(int)0xe00002bcu;
   if(out)*out=0;if(error)*error=0xe00002c7u;return (int)0xe00002c7u;
  }
 }
 if(valid&&(production_mode||getenv("REIMS_HEVC_EXECUTE_GEN12_STREAM")||(getenv("REIMS_HEVC_EXECUTE_GEN12_ONCE")&&atomic_fetch_add(&execute_count,1)==0))&&w[4]>w[2]&&w[12]>w[16]){
  int completed=reims_submit_gen12_probe(shared,(void*)w[2],w[4]-w[2],(void*)w[16],w[12]-w[16],getenv("REIMS_HEVC_CAPTURE_DIR"),io,flags,out,error);
  if(completed==2){atomic_store(&encode_completed,1);return 0;}
  if(completed<0)return error&&*error?(int)*error:(int)0xe00002bcu;
  fprintf(stderr,"HEVC_GEN12_EXECUTION_REJECTED before submit\n");
 }
 if(valid&&getenv("REIMS_HEVC_PREPARE_GEN12")&&w[4]>=w[2]&&w[12]>=w[16]){
  int prepared=reims_prepare_gen12_probe(shared,(void*)w[2],w[4]-w[2],(void*)w[16],w[12]-w[16],getenv("REIMS_HEVC_CAPTURE_DIR"));
  fprintf(stderr,"HEVC_GEN12_PREPARE_RESULT %d\n",prepared);
 }
 if(out)*out=0;if(error)*error=0xe00002c7u;return (int)0xe00002c7u;
}
static void*redirect_dlopen(const char*path,int mode){const char*override=production_mode?production_va:getenv("REIMS_HEVC_ICL_OVERRIDE");if(path&&override&&strstr(path,"AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver")){fprintf(stderr,"HEVC_CAPTURE_REDIRECT_ICL %s\n",override);return dlopen(override,mode);}return dlopen(path,mode);}
static void*capture_resource(void*shared,const void*input,size_t bytes){
 void*result=reims_owned_resource_create(shared,input,bytes);
 Dl_info d={0};dladdr(__builtin_return_address(0),&d);
 if(result&&getenv("REIMS_HEVC_CAPTURE_RESOURCES")&&d.dli_fname&&
    (strstr(d.dli_fname,"ICL-capture-only")||strstr(d.dli_fname,"AppleIntelICLGraphicsVADriver"))){
  captured_shared=shared;
  unsigned n=atomic_fetch_add(&resource_sequence,1);
  void*metadata=IOAccelResourceGetClientShared(result,NULL);
  if(metadata&&bytes>=0x48){uint32_t handle=0,kind=0,id=0;memcpy(&handle,(char*)metadata+0x100,4);memcpy(&kind,input,4);memcpy(&id,(char*)input+0x40,4);if(kind==0x82&&handle<1024)surface_ids[handle]=id;}
  fprintf(stderr,"HEVC_CAPTURE_RESOURCE seq=%u input_bytes=%zu data_bytes=%zu object=%p metadata=%p caller=%lx\n",n,bytes,IOAccelResourceGetDataSize(result),result,metadata,(uintptr_t)__builtin_return_address(0)-(uintptr_t)d.dli_fbase);
  blob(n,"resource-input",(uintptr_t)input,bytes);blob(n,"resource-object",(uintptr_t)result,128);blob(n,"resource-metadata",(uintptr_t)metadata,320);
 }
 return result;
}
void*reims_submit_hook(void){return (void*)blocked_submit;}
void*reims_dlopen_hook(void){return (void*)redirect_dlopen;}
void*reims_resource_hook(void){return (void*)capture_resource;}
#ifndef REIMS_PLUGIN_MODE
/* Isolated diagnostic build only. Calls from this interposing image itself
 * still target native APIs. Production uses private-VA import binding above. */
extern void IOAccelResourceRelease(void*);
extern void IOAccelVideoContextRelease(void*);
extern int IOAccelVideoContextFinishFenceEvent(void*,unsigned);
__attribute__((used)) static struct{const void*replacement;const void*original;}interpose[] __attribute__((section("__DATA,__interpose")))={
 {(void*)blocked_submit,(void*)IOAccelVideoContextSubmitDataBuffers},
 {(void*)redirect_dlopen,(void*)dlopen},
 {(void*)capture_resource,(void*)IOAccelResourceCreate},
 {(void*)reims_owned_resource_release,(void*)IOAccelResourceRelease},
 {(void*)reims_owned_context_release,(void*)IOAccelVideoContextRelease},
 {(void*)reims_owned_finish_fence,(void*)IOAccelVideoContextFinishFenceEvent},
 {(void*)reims_owned_vm_deallocate,(void*)vm_deallocate}};
#endif
