#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>

// Ventura 22G KC ABI, com.apple.AppleGraphicsDeviceControl. The base is real:
// its start/stop own the AGDC device table, plugin registration and user clients.
// KC vtable comparison against IOService confirms exactly these inherited
// overrides: start 0x5c0, stop 0x5c8, terminate 0x600, message 0x730,
// newUserClient 0x770 (plus destructors/getMetaClass supplied by macros).
// free and both init overloads remain IOService implementations. AGDC adds a
// pure vendor method at 0x850, a state wrapper at 0x858, then eight reserves.
// Guest clang vtable dump verified the same slots and 0x110 base size.
struct AGDCClientState_t;
class AppleGraphicsDeviceControl : public IOService {
 OSDeclareAbstractStructors(AppleGraphicsDeviceControl)
 unsigned char agdcPrivate[0x110 - sizeof(IOService)];
public:
 bool start(IOService*) override;
 void stop(IOService*) override;
 IOReturn message(UInt32, IOService*, void*) override;
 bool terminate(IOOptionBits = 0) override;
 IOReturn newUserClient(task_t, void*, UInt32, OSDictionary*, IOUserClient**) override;
 virtual IOReturn vendor_doDeviceAttribute(UInt32, unsigned long*, unsigned long,
    unsigned long*, unsigned long*, IOExternalMethodArguments*) = 0;
 virtual IOReturn vendor_doDeviceAttribute(UInt32, unsigned long*, unsigned long,
    unsigned long*, unsigned long*, AGDCClientState_t*);
 virtual void _RESERVEDAppleGraphicsDeviceControl0();
 virtual void _RESERVEDAppleGraphicsDeviceControl1();
 virtual void _RESERVEDAppleGraphicsDeviceControl2();
 virtual void _RESERVEDAppleGraphicsDeviceControl3();
 virtual void _RESERVEDAppleGraphicsDeviceControl4();
 virtual void _RESERVEDAppleGraphicsDeviceControl5();
 virtual void _RESERVEDAppleGraphicsDeviceControl6();
 virtual void _RESERVEDAppleGraphicsDeviceControl7();
};
static_assert(sizeof(AppleGraphicsDeviceControl) == 0x110, "Ventura AGDC base ABI");

class ReimsADLGraphicsControl : public AppleGraphicsDeviceControl {
 OSDeclareDefaultStructors(ReimsADLGraphicsControl)
 IOService* framebuffer = nullptr;
 volatile UInt32 unsupportedLogCount = 0;
public:
 // Call before attach/start; both services must be actual, attached objects.
 bool setFramebuffer(IOService*);
 bool terminate(IOOptionBits = 0) override;
 void free() override;
 IOReturn vendor_doDeviceAttribute(UInt32, unsigned long*, unsigned long,
    unsigned long*, unsigned long*, IOExternalMethodArguments*) override;
};
// Returned reference belongs to caller. Destroy before releasing its provider.
ReimsADLGraphicsControl* ReimsADLGraphicsControlCreate(IOService* gpu, IOService* fb);
// On failure caller retains ownership and must keep its pointer.
bool ReimsADLGraphicsControlDestroy(ReimsADLGraphicsControl*);
