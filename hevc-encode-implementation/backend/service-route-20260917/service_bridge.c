/* Independent-process routing experiment. Never edits registry or system files. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "../hevc_recovery.h"
static int from_encoder(void*p){Dl_info d={0};return dladdr(p,&d)&&d.dli_fname&&!strcmp(d.dli_fname,"/System/Library/Video/Plug-Ins/AppleGVAHEVCEncoder.bundle/Contents/MacOS/AppleGVAHEVCEncoder");}
static CFTypeRef search(io_registry_entry_t entry,const io_name_t plane,CFStringRef key,CFAllocatorRef allocator,IOOptionBits options){
 CFTypeRef value=IORegistryEntrySearchCFProperty(entry,plane,key,allocator,options);
 if(from_encoder(__builtin_return_address(0))){char name[160]={0};CFStringGetCString(key,name,sizeof(name),kCFStringEncodingUTF8);fprintf(stderr,"NATIVE_ROUTE search key=%s value_type=%lu\n",name,value?CFGetTypeID(value):0UL);
 if(CFEqual(key,CFSTR("IOGVACodec"))&&value&&CFGetTypeID(value)==CFStringGetTypeID()&&CFEqual(value,CFSTR("Gen10"))){CFRelease(value);value=CFRetain(CFSTR("Gen11"));fprintf(stderr,"NATIVE_ROUTE encoder-only Gen10 compatibility dispatched through native Gen11 VA ABI\n");}}
 return value;
}
static CFPropertyListRef plist(CFAllocatorRef allocator,CFReadStreamRef stream,CFIndex length,CFOptionFlags options,CFPropertyListFormat*format,CFErrorRef*error){
 CFPropertyListRef result=CFPropertyListCreateWithStream(allocator,stream,length,options,format,error);
 if(!from_encoder(__builtin_return_address(0))||!result||CFGetTypeID(result)!=CFDictionaryGetTypeID())return result;
 CFTypeRef support=CFDictionaryGetValue(result,CFSTR("System Support"));if(!support||CFGetTypeID(support)!=CFDictionaryGetTypeID())return result;
 io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("IOPlatformExpertDevice"));CFTypeRef board=svc?IORegistryEntryCreateCFProperty(svc,CFSTR("board-id"),kCFAllocatorDefault,0):NULL;if(svc)IOObjectRelease(svc);
 CFStringRef boardString=NULL;if(board&&CFGetTypeID(board)==CFDataGetTypeID()){const UInt8*p=CFDataGetBytePtr(board);CFIndex n=CFDataGetLength(board);while(n&&p[n-1]==0)n--;boardString=CFStringCreateWithBytes(NULL,p,n,kCFStringEncodingUTF8,false);}if(board)CFRelease(board);if(!boardString)return result;
 CFMutableDictionaryRef copy=CFDictionaryCreateMutableCopy(NULL,0,result),sys=CFDictionaryCreateMutableCopy(NULL,0,support);CFTypeRef existing=CFDictionaryGetValue(sys,boardString);CFMutableDictionaryRef model=existing&&CFGetTypeID(existing)==CFDictionaryGetTypeID()?CFDictionaryCreateMutableCopy(NULL,0,existing):CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);int v=3;CFNumberRef number=CFNumberCreate(NULL,kCFNumberIntType,&v);CFDictionarySetValue(model,CFSTR("vendor8bit"),number);CFDictionarySetValue(sys,boardString,model);CFDictionarySetValue(copy,CFSTR("System Support"),sys);fprintf(stderr,"NATIVE_ROUTE in-memory board-specific vendor8bit=3 (VDEnc), original plist unchanged\n");CFRelease(number);CFRelease(model);CFRelease(sys);CFRelease(boardString);CFRelease(result);return copy;
}

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <unistd.h>
#include <os/log.h>
static int rebind(const char *path,const char *symbol,void *replacement){
 const struct mach_header_64 *h=NULL;intptr_t slide=0;
 for(uint32_t i=0;i<_dyld_image_count();i++)if(!strcmp(_dyld_get_image_name(i),path)){h=(const void*)_dyld_get_image_header(i);slide=_dyld_get_image_vmaddr_slide(i);break;}
 if(!h||h->magic!=MH_MAGIC_64)return 0;
 const struct symtab_command *st=NULL;const struct dysymtab_command *dt=NULL;const struct segment_command_64 *link=NULL;
 const struct load_command *lc=(const void*)(h+1);
 for(uint32_t i=0;i<h->ncmds;i++,lc=(const void*)((const char*)lc+lc->cmdsize)){
  if(lc->cmd==LC_SYMTAB)st=(const void*)lc;if(lc->cmd==LC_DYSYMTAB)dt=(const void*)lc;
  if(lc->cmd==LC_SEGMENT_64&&!strcmp(((const struct segment_command_64*)lc)->segname,"__LINKEDIT"))link=(const void*)lc;
 }
 if(!st||!dt||!link)return 0;
 uintptr_t base=slide+link->vmaddr-link->fileoff;const struct nlist_64 *syms=(const void*)(base+st->symoff);const char*str=(const void*)(base+st->stroff);const uint32_t *ind=(const void*)(base+dt->indirectsymoff);int count=0;
 lc=(const void*)(h+1);
 for(uint32_t i=0;i<h->ncmds;i++,lc=(const void*)((const char*)lc+lc->cmdsize))if(lc->cmd==LC_SEGMENT_64){
  const struct segment_command_64 *sg=(const void*)lc;const struct section_64 *sec=(const void*)(sg+1);
  for(uint32_t j=0;j<sg->nsects;j++){uint32_t type=sec[j].flags&SECTION_TYPE;if(type!=S_LAZY_SYMBOL_POINTERS&&type!=S_NON_LAZY_SYMBOL_POINTERS)continue;
   void **slots=(void**)(slide+sec[j].addr);
   for(uint64_t k=0;k<sec[j].size/8;k++){
    if(sec[j].reserved1+k>=dt->nindirectsyms)return 0;uint32_t ix=ind[sec[j].reserved1+k];if(ix&(INDIRECT_SYMBOL_LOCAL|INDIRECT_SYMBOL_ABS))continue;if(ix>=st->nsyms||syms[ix].n_un.n_strx>=st->strsize)return 0;
    if(strcmp(str+syms[ix].n_un.n_strx,symbol))continue;
    uintptr_t page=(uintptr_t)&slots[k]&~((uintptr_t)vm_page_size-1);
    if(vm_protect(mach_task_self(),page,vm_page_size,0,VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY))return 0;
    slots[k]=replacement;
    if(vm_protect(mach_task_self(),page,vm_page_size,0,sg->initprot))return 0;count++;
   }
  }
 }
 fprintf(stderr,"ReimsHEVC bind %s count=%d\n",symbol,count);return count;
}

extern void reims_configure_production(const char*);
extern void *reims_submit_hook(void);
static const char *encoder="/System/Library/Video/Plug-Ins/AppleGVAHEVCEncoder.bundle/Contents/MacOS/AppleGVAHEVCEncoder";
static char va[4096];
static int va_ready;
static void *load_va(const char *path,int mode){
 if(path&&strstr(path,"AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver")){
  void *h=dlopen(va,mode);
  if(!h||!va_ready){os_log_error(OS_LOG_DEFAULT,"ReimsHEVCService VA load failed %{public}s",dlerror());return NULL;}
  return h;
 }
 return dlopen(path,mode);
}
static void added(const struct mach_header *h,intptr_t slide){
 (void)slide;Dl_info info={0};if(!dladdr(h,&info)||!info.dli_fname)return;
 if(!strcmp(info.dli_fname,va)){
  int create=rebind(va,"_IOAccelResourceCreate",reims_owned_resource_create);
  int release=rebind(va,"_IOAccelResourceRelease",reims_owned_resource_release);
  int context=rebind(va,"_IOAccelVideoContextRelease",reims_owned_context_release);
  int finish=rebind(va,"_IOAccelVideoContextFinishFenceEvent",reims_owned_finish_fence);
  int unmap=rebind(va,"_vm_deallocate",reims_owned_vm_deallocate);
  int submit=rebind(va,"_IOAccelVideoContextSubmitDataBuffers",reims_submit_hook());
  va_ready=create>0&&release>0&&context>0&&finish>0&&unmap>0&&submit>0;
  os_log(OS_LOG_DEFAULT,"ReimsHEVCService ownership imports create=%d release=%d context=%d finish=%d unmap=%d submit=%d",create,release,context,finish,unmap,submit);
  os_log(OS_LOG_DEFAULT,"ReimsHEVCService VA bound=%d",va_ready);
 }
 if(!strcmp(info.dli_fname,encoder)){
  int a=rebind(encoder,"_CFPropertyListCreateWithStream",plist);
  int b=rebind(encoder,"_IORegistryEntrySearchCFProperty",search);
  int c=rebind(encoder,"_dlopen",load_va);
  os_log(OS_LOG_DEFAULT,"ReimsHEVCService encoder imports plist=%d registry=%d load=%d",a,b,c);
 }
}
__attribute__((constructor)) static void start(void){
 if(strcmp(getprogname(),"VTEncoderXPCService")&&strcmp(getprogname(),"ReimsHEVCServiceProbe"))return;
 Dl_info info={0};if(!dladdr(start,&info)||!info.dli_fname)return;
 snprintf(va,sizeof(va),"%s",info.dli_fname);char *p=strrchr(va,'/');if(!p)return;
 snprintf(p+1,sizeof(va)-(p+1-va),"NativeHEVCVA");
 reims_configure_production(va);
 os_log(OS_LOG_DEFAULT,"ReimsHEVCService bootstrap pid=%d",getpid());
 _dyld_register_func_for_add_image(added);
}
