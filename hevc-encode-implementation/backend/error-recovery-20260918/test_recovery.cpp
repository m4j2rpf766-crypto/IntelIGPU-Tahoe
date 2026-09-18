#include "../hevc_recovery.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <mach/mach_vm.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <fstream>
#include <set>
extern "C" void *reims_submit_hook(void);
extern "C" void reims_configure_production(const char *);

// All IOAccel entry points below are test doubles. No IOService/user client or
// GPU submission is opened. CF objects, VM mappings and IOSurfaces are real.
struct State {
 int submit=0,device=0,fence=0,drain=0;
 bool allow=false,blockFence=false;
 std::atomic<unsigned> submits{0},fences{0},drains{0},releases{0};
 std::mutex mutex;std::condition_variable cv;
};
static std::atomic<unsigned> nextHandle{1},activities{0};
static State *state(void *io){State *s=nullptr;memcpy(&s,CFDataGetBytePtr((CFDataRef)io),sizeof(s));return s;}
static void *context(State &s){State *p=&s;return (void *)CFDataCreate(nullptr,(const UInt8 *)&p,sizeof(p));}
extern "C" void xpc_transaction_begin(){activities++;}
extern "C" void xpc_transaction_end(){assert(activities>0);activities--;}
extern "C" void *IOAccelResourceCreate(void *,const void *input,size_t n){
 assert(n>=0x60);auto d=CFDataCreateMutable(nullptr,320);CFDataSetLength(d,320);
 auto *p=CFDataGetMutableBytePtr(d);unsigned h=nextHandle++;memcpy(p+0x100,&h,4);
 size_t bytes=0;memcpy(&bytes,(const char *)input+0x18,8);memcpy(p+0x108,&bytes,8);return d;
}
extern "C" void *IOAccelResourceGetClientShared(void *r,void **){return (void *)CFDataGetBytePtr((CFDataRef)r);}
extern "C" size_t IOAccelResourceGetDataSize(void *r){size_t n;memcpy(&n,(const char *)IOAccelResourceGetClientShared(r,nullptr)+0x108,8);return n;}
extern "C" void IOAccelResourceRelease(void *r){if(r)CFRelease(r);}
extern "C" unsigned IOAccelVideoContextGetDataBufferClassCount(void *){return 0;}
extern "C" void *IOAccelVideoContextGetDataBufferResource(void *,unsigned){return nullptr;}
extern "C" void IOAccelVideoContextRelease(void *io){state(io)->releases++;CFRelease(io);}
extern "C" int IOAccelVideoContextSubmitDataBuffers(void *io,unsigned,unsigned *out,unsigned *err){auto *s=state(io);s->submits++;*out=99;*err=s->device;return s->submit;}
extern "C" int IOAccelVideoContextFinishFenceEvent(void *io,unsigned){auto *s=state(io);s->fences++;if(s->blockFence){std::unique_lock<std::mutex> l(s->mutex);s->cv.wait(l,[s]{return s->allow;});}return s->fence;}
extern "C" int IOAccelVideoContextFinish(void *io){
 auto *s=state(io);s->drains++;std::unique_lock<std::mutex> l(s->mutex);
 s->cv.wait(l,[s]{return s->allow;});return s->drain;
}
static void allow(State &s){{std::lock_guard<std::mutex> l(s.mutex);s.allow=true;}s.cv.notify_all();}
template<class F> static void until(F f){for(unsigned i=0;i<1000&&!f();i++)std::this_thread::sleep_for(std::chrono::milliseconds(2));assert(f());}
static bool mapped(uintptr_t p){char c=0;mach_vm_size_t n=0;return mach_vm_read_overwrite(mach_task_self(),p,1,(mach_vm_address_t)&c,&n)==0&&n==1;}
static unsigned handle(void *r){unsigned h;memcpy(&h,(const char *)IOAccelResourceGetClientShared(r,nullptr)+0x100,4);return h;}
struct Native {
 void *resource=nullptr;uintptr_t address=0;size_t bytes=0;
};
static Native resource(void *shared){
 Native r;r.bytes=vm_page_size;mach_vm_address_t a=0;assert(mach_vm_allocate(mach_task_self(),&a,r.bytes,VM_FLAGS_ANYWHERE)==0);r.address=a;
 unsigned char request[0x60]={};uint32_t kind=0x80;memcpy(request,&kind,4);
 memcpy(request+0x18,&r.bytes,8);memcpy(request+0x40,&a,8);memcpy(request+0x48,&a,8);memcpy(request+0x58,&r.bytes,8);
 r.resource=reims_owned_resource_create(shared,request,sizeof(request));assert(r.resource);return r;
}
static std::vector<uint32_t> side(std::initializer_list<unsigned> handles){
 std::vector<uint32_t> b(4);b.push_back(((3+5*handles.size())<<16)|0x8200);b.push_back(0);b.push_back(0);
 for(auto h:handles){b.push_back(0x11);b.push_back(h);b.push_back(0);b.push_back(0);b.push_back(0);}
 b.insert(b.end(),{0x38000,1,1,0x200,1});return b;
}
static ReimsScratch scratch(void *shared){ReimsScratch s={};assert(reims_scratch_create(shared,4096,&s));return s;}
static void drop(Native r){reims_owned_resource_release(r.resource);assert(reims_owned_vm_deallocate(mach_task_self(),r.address,r.bytes)==0);}
static int execute(void *shared,void *io,ReimsScratch &s,const std::vector<uint32_t> &b,unsigned &err){unsigned out=0;return reims_execute_owned_batch(shared,io,0,1,&out,&err,&s,1,b.data(),b.size());}

static std::vector<uint32_t> fixture(const char *p){std::ifstream f(p,std::ios::binary|std::ios::ate);assert(f);auto n=f.tellg();assert(n>0&&n%4==0);std::vector<uint32_t> b(n/4);f.seekg(0);f.read((char *)b.data(),n);assert(f);return b;}
int main(int argc,char **argv){
 assert(argc==1||argc==3);
 void *shared=(void *)0x1234;
 // Replay a captured native batch through the actual production submit hook
 // and converter. Only the final IOAccel calls are replaced with test doubles.
 for(unsigned failure=0;argc==3&&failure<2;failure++){
  auto commands=fixture(argv[1]),sb=fixture(argv[2]);std::set<unsigned> handles;
  for(size_t t=4;t<sb.size();){unsigned n=sb[t]>>16;if(sb[t]==0x200)break;assert(n>=2&&n<=sb.size()-t);
   if((sb[t]&65535)==0x8200)for(size_t r=t+3;r<t+n&&sb[r];r+=5){unsigned op=(sb[r]>>1)&7;handles.insert(sb[r+1]&65535);if(op==2||op==4||op==5||op==6)handles.insert(sb[r+1]>>16);}
   t+=n;
  }
  std::vector<Native> native;for(auto h:handles){assert(h);nextHandle=h;native.push_back(resource(shared));}nextHandle=1000;
  State st;st.submit=failure?int(0xe00002bdu):0;void *io=context(st);uint64_t w[32]={};
  w[0]=(uintptr_t)shared;w[1]=(uintptr_t)io;w[2]=(uintptr_t)commands.data();w[4]=w[2]+commands.size()*4;w[16]=(uintptr_t)sb.data();w[12]=w[16]+sb.size()*4;
  reims_configure_production("test-only");unsigned err=0;auto hook=(int(*)(void*,unsigned,unsigned*,unsigned*))reims_submit_hook();
  int result=hook(io,0,(unsigned *)((char *)w+0x54),&err);assert(st.submits==1);assert(result==st.submit&&err==(unsigned)st.submit);
  for(auto r:native)drop(r);reims_owned_context_release(io);if(failure)allow(st);until([&]{return activities==0;});
 }
 if(argc==3)puts("PASS: optional captured native HEVC batch replay");
 else puts("SKIP: private native command fixture not distributed; synthetic production flush and ownership tests remain enabled");
 // The producer's final flush also owns its native fence resource on error.
 {
  State st;st.fence=45;void *io=context(st);Native r=resource(shared);std::vector<uint32_t> cb(3074),sb(1038);
  cb[0]=0x13004082;cb[3]=1;cb[5]=0x05000000;sb[4]=0x20100;sb[6]=0x30000;sb[9]=0x4008200;sb[12]=0x31;sb[13]=handle(r.resource);sb[1033]=0x38000;sb[1034]=6;sb[1035]=1;sb[1036]=0x200;sb[1037]=3074;
  uint64_t w[32]={};w[0]=(uintptr_t)shared;w[1]=(uintptr_t)io;w[2]=(uintptr_t)cb.data();w[4]=w[2]+cb.size()*4;w[16]=(uintptr_t)sb.data();w[12]=w[16]+sb.size()*4;
  auto hook=(int(*)(void*,unsigned,unsigned*,unsigned*))reims_submit_hook();unsigned err=0;
  assert(hook(io,0,(unsigned *)((char *)w+0x54),&err)==45&&err==45);drop(r);assert(mapped(r.address));reims_owned_context_release(io);allow(st);until([&]{return activities==0;});assert(!mapped(r.address));
 }
 // Success, references and VM reclaimed exactly at confirmed fence completion.
 {
  State st;void *io=context(st);Native r=resource(shared);auto sc=scratch(shared);uintptr_t sa=sc.backing;auto b=side({handle(r.resource),sc.handle});unsigned err=0;
  assert(execute(shared,io,sc,b,err)==2);assert(st.submits==1&&st.fences==1&&st.drains==0);assert(!sc.resource&&!mapped(sa));assert(CFGetRetainCount(r.resource)==1);drop(r);reims_owned_context_release(io);assert(st.releases==2&&activities==0);
 }
 // Both submit return errors and device error words are preserved. Recovery
 // may block without stopping this thread or another context's successful job.
 {
  State st;st.blockFence=true;void *io=context(st);Native r=resource(shared);auto sc=scratch(shared);uintptr_t sa=sc.backing;auto b=side({handle(r.resource),sc.handle});unsigned err=0;int result=0;
  std::thread submit([&]{result=execute(shared,io,sc,b,err);});until([&]{return st.fences==1;});drop(r);reims_owned_context_release(io);assert(mapped(r.address)&&mapped(sa)&&st.releases==1);allow(st);submit.join();assert(result==2&&st.releases==2&&activities==0&&!mapped(r.address)&&!mapped(sa));
 }
 for(unsigned scenario=0;scenario<3;scenario++){
  State st;if(scenario==0)st.submit=int(0xe00002bdu);if(scenario==1)st.device=int(0xe00002c0u);if(scenario==2)st.fence=int(0xe00002d6u);
  void *io=context(st);Native r=resource(shared);auto sc=scratch(shared);uintptr_t sa=sc.backing;auto b=side({handle(r.resource),sc.handle});unsigned err=0;
  auto before=std::chrono::steady_clock::now();assert(execute(shared,io,sc,b,err)==-1);assert(std::chrono::steady_clock::now()-before<std::chrono::seconds(1));
  unsigned expected=st.submit?st.submit:(st.device?st.device:st.fence);assert(err==expected);assert(reims_context_error(io)==int(expected));assert(reims_owned_finish_fence(io,1)==int(expected));
  unsigned calls=st.submits;auto rejected=scratch(shared);assert(execute(shared,io,rejected,b,err)==-1);assert(rejected.resource&&st.submits==calls);reims_scratch_destroy(&rejected);
  drop(r);assert(mapped(r.address)&&mapped(sa));reims_owned_context_release(io);assert(st.releases==1);
  State other;void *oi=context(other);auto os=scratch(shared);auto ob=side({os.handle});assert(execute(shared,oi,os,ob,err)==2);reims_owned_context_release(oi);
  allow(st);until([&]{return activities==0;});assert(st.releases==2);assert(!mapped(r.address)&&!mapped(sa));
 }
 // Namespace mismatch and malformed sideband never reach native submission.
 {
  State st;void *io=context(st);Native r=resource((void *)0x9999);auto sc=scratch(shared);auto b=side({handle(r.resource),sc.handle});unsigned err=0;
  assert(execute(shared,io,sc,b,err)==0);assert(st.submits==0&&sc.resource);b=side({sc.handle});b.pop_back();assert(execute(shared,io,sc,b,err)==0);assert(st.submits==0);reims_scratch_destroy(&sc);drop(r);reims_owned_context_release(io);
 }
 // A paired relocation owns both halves, regardless of bit 31. Missing the
 // second resource is rejected; a declared allocation above budget is refused.
 {
  State st;void *io=context(st);Native a=resource(shared),b=resource(shared);auto sc=scratch(shared);auto sb=side({handle(a.resource)|(handle(b.resource)<<16),sc.handle});sb[7]=5;unsigned err=0;
  assert(execute(shared,io,sc,sb,err)==2);sc=scratch(shared);sb[8]=handle(a.resource)|(65000u<<16);assert(execute(shared,io,sc,sb,err)==0);assert(st.submits==1);
  size_t actual=sc.bytes;sc.bytes=512u*1024u*1024u+1;assert(execute(shared,io,sc,side({sc.handle}),err)==0);assert(st.submits==1);sc.bytes=actual;reims_scratch_destroy(&sc);drop(a);drop(b);reims_owned_context_release(io);
 }
 // Surface use count (not just retain count) prevents pool reuse while a
 // failed command can still access the surface. Real CPU-only IOSurface.
 {
  int width=16,height=16,bpe=4;auto dict=CFDictionaryCreateMutable(nullptr,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
  CFStringRef keys[]={kIOSurfaceWidth,kIOSurfaceHeight,kIOSurfaceBytesPerElement};int vals[]={width,height,bpe};
  for(int i=0;i<3;i++){auto n=CFNumberCreate(nullptr,kCFNumberIntType,&vals[i]);CFDictionarySetValue(dict,keys[i],n);CFRelease(n);}
  IOSurfaceRef surface=IOSurfaceCreate(dict);CFRelease(dict);assert(surface);int use=IOSurfaceGetUseCount(surface);
  uint8_t req[0x60]={};uint32_t kind=0x82,sid=IOSurfaceGetID(surface);memcpy(req,&kind,4);memcpy(req+0x40,&sid,4);
  void *r=reims_owned_resource_create(shared,req,sizeof(req));assert(r);
  State st;st.fence=int(0xe00002d6u);void *io=context(st);auto sc=scratch(shared);auto b=side({handle(r),sc.handle});unsigned err=0;
  assert(execute(shared,io,sc,b,err)==-1);assert(IOSurfaceGetUseCount(surface)==use+1);reims_owned_resource_release(r);reims_owned_context_release(io);allow(st);until([&]{return activities==0;});assert(IOSurfaceGetUseCount(surface)==use);CFRelease(surface);
 }
 // Once drain succeeds the old context stays poisoned until it is released.
 {
  State st;st.device=42;st.allow=true;void *io=context(st);auto sc=scratch(shared);auto b=side({sc.handle});unsigned err=0;
  assert(execute(shared,io,sc,b,err)==-1);until([&]{return st.drains>0;});std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(reims_context_error(io)==42);reims_owned_context_release(io);until([&]{return activities==0;});
 }
 puts("PASS: success; submit/device/fence failures; nonblocking error propagation; per-context isolation; deferred VM and resource release; namespace and sideband rejection; IOSurface use-count lifetime; failed-context reuse refused");
 // Persistent drain failure must NOT be reinterpreted as completion. The
 // bounded owner intentionally survives until process teardown in this test.
 auto *st=new State;st->submit=43;st->drain=44;st->allow=true;void *io=context(*st);Native r=resource(shared);auto sc=scratch(shared);uintptr_t sa=sc.backing;auto b=side({handle(r.resource),sc.handle});unsigned err=0;
 assert(execute(shared,io,sc,b,err)==-1);drop(r);reims_owned_context_release(io);until([&]{return st->drains==3;});assert(mapped(r.address)&&mapped(sa));assert(st->releases==1&&activities==1);
 puts("PASS: three failed native drain attempts retain resources and XPC activity; no false completion or forced cleanup");
 // Exhaust all 16 retained-context slots; the next request returns promptly
 // without submission. Already-failed contexts cannot grow the retained set.
 for(unsigned i=1;i<16;i++){
  auto *s=new State;s->submit=43;s->drain=44;s->allow=true;void *ci=context(*s);auto scratchBuffer=scratch(shared);auto refs=side({scratchBuffer.handle});
  assert(execute(shared,ci,scratchBuffer,refs,err)==-1);reims_owned_context_release(ci);until([&]{return s->drains==3;});
 }
 State overflow;void *oi=context(overflow);auto os=scratch(shared);auto ob=side({os.handle});assert(execute(shared,oi,os,ob,err)==-1&&overflow.submits==0&&os.resource);reims_scratch_destroy(&os);reims_owned_context_release(oi);assert(activities==16);
 puts("PASS: final flush failure, paired relocation ownership, 512 MiB preflight budget and 16-context quarantine cap");
}
