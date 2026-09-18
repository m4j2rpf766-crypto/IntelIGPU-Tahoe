// Experimental: restore the driver's own delete branch for orphaned Posh.
// No completion hooks, external free, or changes while a CB is usable.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <os/log.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
static ptrdiff_t poshOff, onlyOff, queueOff, sharedOff;
static void (*nativeDealloc)(id, SEL);
static _Atomic unsigned long long count;
static void *ptr(id o, ptrdiff_t off) { return *(void **)((char *)(void *)o+off); }
static void cleanup(id o, SEL s) {
 id q = ptr(o, queueOff);
 if (q && ptr(o, poshOff) && !ptr(q, sharedOff) &&
     *(unsigned char *)((char *)(void *)o+onlyOff)) {
  *(unsigned char *)((char *)(void *)o+onlyOff) = 0;
  unsigned long long n = atomic_fetch_add(&count, 1)+1;
  if (!(n & 4095)) os_log(OS_LOG_DEFAULT,"Reims Posh native destructor count=%{public}llu",n);
 }
 nativeDealloc(o,s);
}
__attribute__((constructor)) static void install(void) {
 if (strcmp(getprogname(),"WindowServer")) return;
 Class c=objc_getClass("MTLIGAccelCommandBuffer");
 Class q=objc_getClass("MTLIGAccelCommandQueue");
 if (!c || !q) return;
 Ivar p=class_getInstanceVariable(c,"_pPoshUsedAsTarget");
 Ivar a=class_getInstanceVariable(c,"m_isOnlyActiveCommandBuffer");
 Ivar k=class_getInstanceVariable(c,"_pIGCommandQueue");
 Ivar h=class_getInstanceVariable(q,"m_pPoshUsedAsTarget");
 if (!p || !a || !k || !h || strcmp(ivar_getTypeEncoding(a),"B")) return;
 poshOff=ivar_getOffset(p); onlyOff=ivar_getOffset(a);
 queueOff=ivar_getOffset(k); sharedOff=ivar_getOffset(h);
 if (poshOff!=816 || onlyOff!=880 || sharedOff!=848) return;
 Method m=class_getInstanceMethod(c,sel_registerName("dealloc"));
 if (!m) return;
 nativeDealloc=(void (*)(id,SEL))method_getImplementation(m);
 method_setImplementation(m,(IMP)cleanup);
 os_log(OS_LOG_DEFAULT,"Reims Posh native destructor installed");
}
