#include "hevc_recovery.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <pthread.h>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <limits>

extern "C" {
void *IOAccelResourceCreate(void *,const void *,size_t);
void *IOAccelResourceGetClientShared(void *,void **);
size_t IOAccelResourceGetDataSize(void *);
void IOAccelResourceRelease(void *);
unsigned IOAccelVideoContextGetDataBufferClassCount(void *);
void *IOAccelVideoContextGetDataBufferResource(void *,unsigned);
int IOAccelVideoContextSubmitDataBuffers(void *,unsigned,unsigned *,unsigned *);
int IOAccelVideoContextFinishFenceEvent(void *,unsigned);
int IOAccelVideoContextFinish(void *);
void IOAccelVideoContextRelease(void *);
/* Existing libSystem transaction API: keep XPC's idle exit from tearing down
 * an outstanding recovery owner. Never end it on an unconfirmed completion. */
void xpc_transaction_begin(void);
void xpc_transaction_end(void);
}

namespace {
constexpr unsigned kResources=4096,kContexts=16,kHeld=256,kDeferred=4096;
constexpr size_t kRetainedBudget=512u*1024u*1024u;
constexpr int kFailure=int(0xe00002bcu),kBusy=int(0xe00002d5u);
struct Resource {
 void *object=nullptr,*shared=nullptr;
 uint32_t handle=0;
 uintptr_t address=0;
 size_t bytes=0;
 IOSurfaceRef surface=nullptr; // catalog owns this reference, not a use count
 bool supported=false;
};
struct Hold {void *object=nullptr;uintptr_t address=0;size_t bytes=0;IOSurfaceRef surface=nullptr;};
struct Context {
 void *io=nullptr;
 bool busy=false,failed=false,released=false,recovering=false,activity=false;
 int error=0;
 Hold held[kHeld]{};
 unsigned heldCount=0;
 ReimsScratch scratch[11]{};
 unsigned scratchCount=0;
 size_t chargedBytes=0;
};
struct Deferred {vm_address_t address=0;vm_size_t bytes=0;};
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
Resource resources[kResources];
Context contexts[kContexts];
Deferred deferred[kDeferred];
size_t reservedBytes=0;
struct Lock {Lock(){pthread_mutex_lock(&mutex);}~Lock(){pthread_mutex_unlock(&mutex);}};
Context *find(void *io){for(auto &c:contexts)if(c.io==io)return &c;return nullptr;}
bool overlaps(uintptr_t a,size_t an,uintptr_t b,size_t bn){
 return an&&bn&&a<=UINTPTR_MAX-an&&b<=UINTPTR_MAX-bn&&a<b+bn&&b<a+an;
}
bool heldRange(uintptr_t a,size_t n){
 for(const auto &c:contexts)for(unsigned i=0;i<c.heldCount;i++)
  if(overlaps(a,n,c.held[i].address,c.held[i].bytes))return true;
 return false;
}
bool addHold(Context &c,void *object,const Resource *r=nullptr){
 if(!object)return false;
 for(unsigned i=0;i<c.heldCount;i++)if(c.held[i].object==object)return true;
 if(c.heldCount==kHeld|| (r&&!r->supported))return false;
 size_t charge=r&&r->surface?IOSurfaceGetAllocSize(r->surface):IOAccelResourceGetDataSize(object);
 if(r&&r->bytes>charge)charge=r->bytes;
 if(charge>kRetainedBudget-reservedBytes)return false;
 reservedBytes+=charge;c.chargedBytes+=charge;
 Hold &h=c.held[c.heldCount++];h.object=(void *)CFRetain(object);
 if(r){
  h.address=r->address;h.bytes=r->bytes;
  if(r->surface){h.surface=(IOSurfaceRef)CFRetain(r->surface);IOSurfaceIncrementUseCount(h.surface);}
 }
 return true;
}
bool addHandle(Context &c,void *shared,uint32_t handle){
 if(!handle)return false;
 for(unsigned i=0;i<c.scratchCount;i++)if(c.scratch[i].handle==handle)return true;
 for(const auto &r:resources)if(r.object&&r.shared==shared&&r.handle==handle)return addHold(c,r.object,&r);
 // Data-buffer resources are owned by the retained native context, and their
 // framework allocation does not pass through the VA's resource-create import.
 for(unsigned i=0;i<c.heldCount;i++){
  auto *meta=(const unsigned char *)IOAccelResourceGetClientShared(c.held[i].object,nullptr);
  uint32_t h=0;if(meta)memcpy(&h,meta+0x100,4);if(h==handle)return true;
 }
 fprintf(stderr,"HEVC_RECOVERY unknown_resource handle=%u; submit rejected\n",handle);
 return false;
}
/* Drop resource references before their deferred vm mappings. Serialize the
 * catalog/free hooks, but NEVER hold this lock across a GPU completion wait.
 * Context destruction is separately done outside the lock. */
void releaseHolds(Context &c){
 for(unsigned i=0;i<c.heldCount;i++){
  auto &h=c.held[i];
  IOAccelResourceRelease(h.object);
  if(h.surface){IOSurfaceDecrementUseCount(h.surface);CFRelease(h.surface);}
  h={};
 }
 c.heldCount=0;
 for(auto &d:deferred)if(d.bytes&&!heldRange(d.address,d.bytes)){
  kern_return_t rc=vm_deallocate(mach_task_self(),d.address,d.bytes);
  if(rc==KERN_SUCCESS)d={};
  else fprintf(stderr,"HEVC_RECOVERY deferred_unmap_failed rc=%x\n",rc);
 }
 for(unsigned i=0;i<c.scratchCount;i++)reims_scratch_destroy(&c.scratch[i]);
 c.scratchCount=0;
 reservedBytes-=c.chargedBytes;c.chargedBytes=0;
}
void *recover(void *arg){
 auto *c=(Context *)arg;
 // Full-context drain, not a potentially stale/reused fence slot. A submit
 // error alone does not establish that no command was accepted by the kernel.
 int result=kFailure;
 for(unsigned attempt=0;attempt<3;attempt++){
  if(attempt){
#ifdef REIMS_RECOVERY_TEST
   usleep(1000);
#else
   sleep(attempt==1?1:5);
#endif
  }
  result=IOAccelVideoContextFinish(c->io);
  if(!result)break;
 }
 void *release=nullptr;bool endActivity=false;
 {
  Lock lock;
  if(!result){
   releaseHolds(*c);c->recovering=false;
   if(c->released){release=c->io;endActivity=c->activity;*c={};}
   // A failed context stays poisoned until its native owner releases it.
   // Never allow fence-slot reuse to turn an earlier failure into success.
  }else{
   c->recovering=false;
   fprintf(stderr,"HEVC_RECOVERY retained completion_unknown error=%x; bounded slots remain occupied\n",result);
  }
 }
 if(release)IOAccelVideoContextRelease(release);
 if(endActivity)xpc_transaction_end();
 return nullptr;
}
}

extern "C" void *reims_owned_resource_create(void *shared,const void *input,size_t bytes){
 void *object=IOAccelResourceCreate(shared,input,bytes);if(!object)return nullptr;
 auto *meta=(const unsigned char *)IOAccelResourceGetClientShared(object,nullptr);
 Resource r;r.object=object;r.shared=shared;
 if(meta)memcpy(&r.handle,meta+0x100,4);
 if(input&&bytes>=0x60){
  uint32_t kind=0;memcpy(&kind,input,4);
  if(kind==0x80){
   uintptr_t second=0;memcpy(&r.address,(const char *)input+0x40,8);
   memcpy(&second,(const char *)input+0x48,8);memcpy(&r.bytes,(const char *)input+0x58,8);
   // Pinned ICL sysmem is vm_allocate-backed, and released via vm_deallocate.
   r.supported=r.address==second&&r.address&&r.bytes&&r.bytes<=UINTPTR_MAX-r.address;
  }else if(kind==0x82){
   uint32_t sid=0;memcpy(&sid,(const char *)input+0x40,4);
   r.surface=sid?IOSurfaceLookup(sid):nullptr;r.supported=r.surface!=nullptr;
  }
 }
 bool added=false;
 {Lock lock;for(auto &slot:resources)if(!slot.object){slot=r;added=true;break;}}
 if(!added){if(r.surface)CFRelease(r.surface);IOAccelResourceRelease(object);return nullptr;}
 return object;
}
extern "C" void reims_owned_resource_release(void *object){
 if(!object)return;
 {Lock lock;for(auto &r:resources)if(r.object==object){if(r.surface)CFRelease(r.surface);r={};break;}}
 IOAccelResourceRelease(object);
}
extern "C" kern_return_t reims_owned_vm_deallocate(vm_map_t task,vm_address_t address,vm_size_t bytes){
 Lock lock;
 if(task==mach_task_self()&&heldRange(address,bytes)){
  for(const auto &d:deferred)if(d.address==address&&d.bytes==bytes)return KERN_SUCCESS;
  for(auto &d:deferred)if(!d.bytes){d={address,bytes};return KERN_SUCCESS;}
  // Refuse rather than unmap in-use memory or lose ownership bookkeeping.
  fprintf(stderr,"HEVC_RECOVERY deferred_unmap_capacity_exhausted\n");return KERN_RESOURCE_SHORTAGE;
 }
 return vm_deallocate(task,address,bytes);
}
extern "C" int reims_context_error(void *io){
 Lock lock;auto *c=find(io);return c?(c->failed?c->error:(c->busy?kBusy:0)):0;
}
extern "C" int reims_owned_finish_fence(void *io,unsigned event){
 int error=reims_context_error(io);return error?error:IOAccelVideoContextFinishFenceEvent(io,event);
}
extern "C" void reims_owned_context_release(void *io){
 void *extra=nullptr;bool activity=false;
 {
  Lock lock;auto *c=find(io);
  if(c){
   c->released=true;
   if(!c->busy&&!c->recovering&&!c->heldCount&&!c->scratchCount){extra=c->io;activity=c->activity;*c={};}
  }
 }
 // Native ownership and our additional retain are distinct and each is
 // relinquished exactly once. An uncertain context keeps our retain alive.
 if(io)IOAccelVideoContextRelease(io);
 if(extra)IOAccelVideoContextRelease(extra);
 if(activity)xpc_transaction_end();
}
extern "C" int reims_execute_owned_batch(void *shared,void *io,unsigned flags,unsigned event,
 unsigned *out,unsigned *error,ReimsScratch *scratch,unsigned count,const uint32_t *side,size_t words){
 if(!shared||!io||!out||!error||(count&&!scratch)||count>11||!side||words<6)return 0;
 Context *c=nullptr;bool acquired=false;
 {
  Lock lock;c=find(io);
  if(c&&(c->failed||c->busy)){*out=0;*error=(unsigned)(c->failed?c->error:kBusy);return -1;}
  if(!c){for(auto &slot:contexts)if(!slot.io){c=&slot;c->io=(void *)CFRetain(io);break;}}
  if(!c){*out=0;*error=(unsigned)kBusy;return -1;}
  c->busy=true;
  size_t charge=0;
  for(unsigned i=0;i<count;i++){
   if(scratch[i].bytes>kRetainedBudget-charge){c->busy=false;return 0;}
   charge+=scratch[i].bytes;
  }
  if(charge>kRetainedBudget-reservedBytes){c->busy=false;return 0;}
  reservedBytes+=charge;c->chargedBytes=charge;
  // Borrow scratch for handle lookup; ownership remains with the caller on
  // any preflight rejection. No resource can be submitted without a slot.
  c->scratchCount=count;if(count)memcpy(c->scratch,scratch,count*sizeof(*scratch));
  acquired=true;
  unsigned classes=IOAccelVideoContextGetDataBufferClassCount(io);
  if(classes>16)acquired=false;
  for(unsigned i=0;acquired&&i<classes;i++){
   void *r=IOAccelVideoContextGetDataBufferResource(io,i);
   if(r&&!addHold(*c,r))acquired=false;
  }
  bool terminal=false;
  for(size_t t=4;acquired&&t<words;){
   uint32_t token=side[t],n=token>>16;
   if(token==0x200){terminal=t+2==words;break;}
   if(n<2||n>words-t){acquired=false;break;}
   if((token&65535)==0x8200){
    if(n<3){acquired=false;break;}
    for(size_t r=t+3;r<t+n;){
     if(!side[r]){for(size_t z=r;z<t+n;z++)if(side[z])acquired=false;break;}
     if(t+n-r<5||!(side[r]&1)){acquired=false;break;}
     // Native patch_codechal_command_buffer binds the low 16-bit handle
     // first (0x77bb1), then the high half for operations 2,4,5,6.
     // Bit 31 describes tiled addressing, NOT whether a second handle exists.
     unsigned operation=(side[r]>>1)&7,handles=side[r+1];
     if(operation==7||!addHandle(*c,shared,handles&65535)||
        ((operation==2||operation==4||operation==5||operation==6)&&
         !addHandle(*c,shared,handles>>16))){acquired=false;break;}
     r+=5;
    }
   }
   t+=n;
  }
  acquired=acquired&&terminal;
  if(!acquired){c->scratchCount=0;memset(c->scratch,0,sizeof(c->scratch));releaseHolds(*c);c->busy=false;return 0;}
  if(count)memset(scratch,0,count*sizeof(*scratch));
  xpc_transaction_begin();c->activity=true;
 }
 int result=IOAccelVideoContextSubmitDataBuffers(io,flags,out,error);
 int finish=(!result&&!*error)?IOAccelVideoContextFinishFenceEvent(io,event):kFailure;
 fprintf(stderr,"HEVC_RECOVERY submit=%x device=%x finish=%x event=%u\n",result,*error,finish,event);
 if(!result&&!*error&&!finish){
  void *release=nullptr;
  {Lock lock;releaseHolds(*c);c->busy=false;c->activity=false;if(c->released){release=c->io;*c={};}}
  if(release)IOAccelVideoContextRelease(release);
  xpc_transaction_end();return 2;
 }
 {
  Lock lock;c->busy=false;c->failed=true;
  c->error=result?result:(*error?int(*error):finish);if(!c->error)c->error=kFailure;
  *out=0;*error=(unsigned)c->error;c->recovering=true;
  fprintf(stderr,"HEVC_RECOVERY failed error=%x resources=%u scratch=%u; native error returned\n",c->error,c->heldCount,c->scratchCount);
 }
 pthread_t thread;
 if(pthread_create(&thread,nullptr,recover,c)==0)pthread_detach(thread);
 else {Lock lock;c->recovering=false;fprintf(stderr,"HEVC_RECOVERY worker_unavailable retained\n");}
 return -1;
}
