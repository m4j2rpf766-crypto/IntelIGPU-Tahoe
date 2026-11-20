#ifndef WEX_HEVC_NATIVE_BACKEND_H
#define WEX_HEVC_NATIVE_BACKEND_H

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include "wire.h"

/* Adapter contract for a real Intel Gen12 HEVC command producer and IOAccel
 * submission implementation. A probe must be read-only and may not submit GPU
 * work. Encode must synchronously return one retained (+1) real HEVC sample.
 * Implementations must report only capabilities they have verified. */
#ifdef __cplusplus
extern "C" {
#endif

OSStatus WEXNativeHEVCProbe(WEXProbeInfo *result);
OSStatus WEXNativeHEVCOpen(const WEXConfig *config,
                           const WEXProbeInfo *probe,
                           void **session);
OSStatus WEXNativeHEVCEncode(void *session,
                             CVPixelBufferRef input,
                             CMTime presentationTime,
                             CMTime duration,
                             uint32_t forceKeyFrame,
                             CMSampleBufferRef *encodedSample);
void WEXNativeHEVEClose(void *session);

#ifdef __cplusplus
}
#endif

#endif
