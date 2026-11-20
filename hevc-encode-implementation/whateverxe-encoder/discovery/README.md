# WEX Intel HEVC Metal route

This opt-in kext changes exactly one property on a matched `IntelAccelerator`:
`MetalPluginName` from Apple's native TGL Metal bundle to the fixed
`WEXHEVCMetalDriver` bundle. It does not publish HEVC admission/profile data,
VA properties, a generic registry writer, or service hooks.

The match is limited to macOS build `25G422`, Intel PCI `8086:4680` with memory
decoding enabled, active `AppleIntelTGLGraphics` accelerator identities, and
renderer ID `0x01080080`. Installation also requires the original native
`MetalPluginName` and no `MetalPluginClassName`. Unsupported states are no-ops.

## Offline build

Run `./build.sh`. It writes only to `${WEX_METAL_ROUTE_BUILD_ROOT}` or the
temporary build directory and ad-hoc signs the resulting bundle locally. It
does not install, load, alter system policy, or reboot. The signed bundle is a
development artifact; normal `kmutil` policy decides whether it can load.

## Load and rollback constraints

The route matches the active accelerator when IOKit processes this personality.
Loading the kext does not restart the Apple accelerator or any process that may
already have cached its Metal plugin. The property change therefore proves only
that the registry selection changed; it does not prove that an existing
VideoProc process loaded the WEX bundle. Verify a fresh client process and the
actual loaded image before treating route selection as effective.

The local `kmutil` interface supports `kmutil load --bundle-path <bundle>`; its
default load style is `start-and-match`, which starts the kext and matches its
personality against existing services. For rollback, use
`kmutil unload --bundle-identifier org.geekaudio.WEXIntelHEVCMetalRoute`;
unload terminates the route service and removes its personalities, allowing
`stop()` to restore the original value. Both operations remain subject to
normal authorization and host policy. This kext does not request an
authorization bypass or change those policies.

On kext stop, the route restores the original property only while the property
still points to the exact `OSString` object published by this instance. If a
different publisher has replaced it, rollback leaves that value untouched.
IOKit's public property API has no atomic compare-and-swap, so provider
arbitration serializes this kext's lifecycle while identity checking prevents
ordinary replacement from being overwritten. The route cannot promise atomic
coordination with unrelated code that ignores provider arbitration.

If normal policy refuses the kext or requires an unsupported signing or boot
change, stop there. Do not change SIP, AMFI, boot arguments, or system services
to make this route load.
