#include <mach/kmod.h>

extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);

KMOD_EXPLICIT_DECL(org.geekaudio.WEXIntelHEVCMetalRoute, "0.1.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = _start;
__private_extern__ kmod_stop_func_t *_antimain = _stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}
