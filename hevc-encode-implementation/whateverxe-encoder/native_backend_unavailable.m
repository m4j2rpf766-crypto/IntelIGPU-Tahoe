#include "native_backend.h"
#include <stdio.h>

/* The current upstream tree does not contain the Gen12 HEVC command producer
 * and IOAccel adapter. Keep the worker buildable while making that missing
 * backend an explicit runtime failure. */
OSStatus WEXNativeHEVCProbe(WEXProbeInfo *result) {
    if (result) *result = (WEXProbeInfo){0};
    fprintf(stderr, "WEX_WORKER no native Intel HEVC adapter is linked\n");
    return kVTVideoEncoderNotAvailableNowErr;
}

OSStatus WEXNativeHEVCOpen(const WEXConfig *config,
                           const WEXProbeInfo *probe,
                           void **session) {
    (void)config;
    (void)probe;
    if (session) *session = NULL;
    return kVTVideoEncoderNotAvailableNowErr;
}

OSStatus WEXNativeHEVCEncode(void *session,
                             CVPixelBufferRef input,
                             CMTime presentationTime,
                             CMTime duration,
                             uint32_t forceKeyFrame,
                             CMSampleBufferRef *encodedSample) {
    (void)session;
    (void)input;
    (void)presentationTime;
    (void)duration;
    (void)forceKeyFrame;
    if (encodedSample) *encodedSample = NULL;
    return kVTVideoEncoderNotAvailableNowErr;
}

void WEXNativeHEVEClose(void *session) {
    (void)session;
}
