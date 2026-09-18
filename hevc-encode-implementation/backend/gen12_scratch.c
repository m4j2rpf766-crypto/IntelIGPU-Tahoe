#include "gen12_scratch.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <string.h>

extern void *IOAccelResourceCreate(void*,const void*,size_t);
extern size_t IOAccelResourceGetDataSize(void*);
extern void *IOAccelResourceGetClientShared(void*,void**);
extern void IOAccelResourceRelease(void*);

static void put16(unsigned char*p,uint16_t v){memcpy(p,&v,2);}
static void put64(unsigned char*p,uint64_t v){memcpy(p,&v,8);}
int reims_scratch_create(void *shared,size_t requested,ReimsScratch*out){
    if(!shared||!out||!requested||requested>64u*1024*1024)return 0;
    size_t page=vm_page_size,bytes=(requested+page-1)&~(page-1);
    mach_vm_address_t address=0;
    if(mach_vm_allocate(mach_task_self(),&address,bytes,VM_FLAGS_ANYWHERE)!=KERN_SUCCESS)return 0;
    // Exactly the linear sysmem-backed request produced by pinned ICL
    // allocator 0x584b0, confirmed by capture on Tahoe. It is not an IOSurface
    // request. Width is a 16-bit hint; 64-bit allocation/backing sizes govern
    // buffers larger than 64 KiB, as in the native producer.
    unsigned char request[0xec8]={0};
    request[0]=0x80;
    put16(request+8,(uint16_t)bytes);put16(request+10,1);put16(request+12,1);
    put64(request+0x18,bytes);
    request[0x20]=1;request[0x21]=1;request[0x23]=1;
    put64(request+0x40,address);put64(request+0x48,address);put64(request+0x58,bytes);
    void *resource=IOAccelResourceCreate(shared,request,sizeof(request));
    if(!resource){mach_vm_deallocate(mach_task_self(),address,bytes);return 0;}
    unsigned char*meta=IOAccelResourceGetClientShared(resource,NULL);
    uint32_t handle=0;uint64_t mapped_size=0;
    if(meta){memcpy(&handle,meta+0x100,4);memcpy(&mapped_size,meta+0x108,8);}
    if(!meta||!handle||handle>65535||IOAccelResourceGetDataSize(resource)!=bytes||mapped_size<bytes){
        IOAccelResourceRelease(resource);mach_vm_deallocate(mach_task_self(),address,bytes);return 0;
    }
    *out=(ReimsScratch){resource,(uintptr_t)address,bytes,handle};return 1;
}
void reims_scratch_destroy(ReimsScratch*r){
    if(!r)return;
    if(r->resource)IOAccelResourceRelease(r->resource);
    if(r->backing&&r->bytes)mach_vm_deallocate(mach_task_self(),r->backing,r->bytes);
    memset(r,0,sizeof(*r));
}
