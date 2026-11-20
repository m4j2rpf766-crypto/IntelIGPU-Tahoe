#ifndef WEX_HEVC_TRANSPORT_H
#define WEX_HEVC_TRANSPORT_H

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct WEXEncoderBackend WEXEncoderBackend;

/* Runs one cached, no-frame native readiness probe through the adjacent worker. */
bool WEXBackendIsAvailable(uint64_t registryID);

OSStatus WEXBackendOpen(uint64_t registryID,
                        int32_t width, int32_t height, int32_t bitrate,
                        int32_t fpsNum, int32_t fpsDen,
                        WEXEncoderBackend **backend);

OSStatus WEXBackendFrame(WEXEncoderBackend *backend,
                         CVPixelBufferRef pixelBuffer,
                         CMTime pts, CMTime duration,
                         bool forceKeyFrame,
                         CMSampleBufferRef *encoded);

void WEXBackendClose(WEXEncoderBackend *backend);

#endif
