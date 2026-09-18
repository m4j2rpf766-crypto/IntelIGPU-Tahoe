#include <mach/kmod.h>

extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
KMOD_EXPLICIT_DECL(lab.reims.ReimsADLDesktopLink, "0.6.23", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = nullptr;
__private_extern__ kmod_stop_func_t *_antimain = nullptr;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}
