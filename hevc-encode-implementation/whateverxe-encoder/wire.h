#ifndef WEX_HEVC_WIRE_H
#define WEX_HEVC_WIRE_H

#include <stdint.h>

/* Local socketpair between matching driver and worker builds, using host byte order.
 * The worker reads and writes descriptor 0. stdout and stderr are never protocol bytes.
 * Each message starts with WEXMessage. A reply repeats the request's kind. An error
 * reply has a nonzero OSStatus and no payload. Closing the socket ends the session.
 * WEXProbe is a no-frame readiness request and returns WEXProbeInfo on success. */
enum {
    WEXWireMagic = 0x57484556,
    WEXWireVersion = 2,
    WEXOpen = 1,
    WEXFrame = 2,
    WEXProbe = 3,
    WEXNativeProbeABIVersion = 1,
    WEXMaxWidth = 1920,
    WEXMaxHeight = 1080,
    WEXMaxPacket = 16 * 1024 * 1024,
    WEXMaxParameterSet = 65535,
    WEXIntelVendorID = 0x8086,
    WEXUHD770DeviceID = 0x4680
};

enum {
    WEXColorCodeUnspecified = UINT32_MAX,
    WEXChromaCodeCenter = 1,
    WEXChromaCodeLeft = 2,
    WEXChromaCodeTopLeft = 3,
    WEXChromaCodeTop = 4,
    WEXChromaCodeBottomLeft = 5,
    WEXChromaCodeBottom = 6,
    WEXChromaCodeDV420 = 7
};

enum {
    WEXProbeCapabilityHEVCMain = 1u << 0,
    WEXProbeCapabilityNV12VideoRange = 1u << 1,
    WEXProbeCapabilityHardwareEncode = 1u << 2,
    WEXProbeCapabilityForceKeyFrame = 1u << 3,
    WEXProbeRequiredCapabilities = (1u << 4) - 1
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t payloadBytes;
    int32_t status;
} WEXMessage;

/* An Open request carries just this configuration. Its success reply is empty. */
typedef struct {
    int32_t width, height, bitrate, fpsNum, fpsDen;
    uint64_t registryID;
} WEXConfig;

/* CoreVideo color-space codepoints. Unmarked attachments use the sentinel. */
typedef struct {
    uint32_t colorPrimaries;
    uint32_t transferFunction;
    uint32_t yCbCrMatrix;
    uint32_t chromaLocationTop;
    uint32_t chromaLocationBottom;
    uint32_t reserved;
} WEXColorMetadata;

/* Timestamp scales of zero represent invalid times. Valid times use epoch zero. */
typedef struct {
    int64_t ptsValue, durationValue;
    int32_t ptsScale, durationScale;
    uint32_t forceKeyFrame, reserved;
    WEXColorMetadata color;
} WEXFrameInfo;
/* The frame payload is WEXFrameInfo, then width*height bytes for Y, then
 * width*height/2 bytes of interleaved UV. Input is video-range NV12. */

/* On success the frame reply is WEXEncodedInfo followed, in order, by the
 * complete VPS, SPS and PPS NAL units (without start codes), then the coded
 * access unit with nalLengthBytes-byte big-endian NAL lengths.
 * Timing is taken from the worker's actual sample buffer. */
typedef struct {
    int64_t ptsValue, durationValue;
    int32_t ptsScale, durationScale;
    uint32_t keyFrame, nalLengthBytes;
    uint32_t vpsBytes, spsBytes, ppsBytes, codedBytes;
} WEXEncodedInfo;

/* A successful probe certifies that the no-submit native adapter found the
 * qualified physical GPU, confirmed its ABI and verified these capabilities. */
typedef struct {
    uint32_t abiVersion;
    uint32_t structBytes;
    uint32_t vendorID;
    uint32_t deviceID;
    uint64_t registryID;
    uint32_t capabilities;
    uint32_t maxWidth;
    uint32_t maxHeight;
    uint32_t reserved;
} WEXProbeInfo;

#if defined(__cplusplus)
#define WEX_STATIC_ASSERT static_assert
#else
#define WEX_STATIC_ASSERT _Static_assert
#endif
WEX_STATIC_ASSERT(sizeof(WEXMessage) == 16, "WEXMessage ABI");
WEX_STATIC_ASSERT(sizeof(WEXConfig) == 32, "WEXConfig ABI");
WEX_STATIC_ASSERT(sizeof(WEXColorMetadata) == 24, "WEXColorMetadata ABI");
WEX_STATIC_ASSERT(sizeof(WEXFrameInfo) == 56, "WEXFrameInfo ABI");
WEX_STATIC_ASSERT(sizeof(WEXEncodedInfo) == 48, "WEXEncodedInfo ABI");
WEX_STATIC_ASSERT(sizeof(WEXProbeInfo) == 40, "WEXProbeInfo ABI");
#undef WEX_STATIC_ASSERT
#endif
