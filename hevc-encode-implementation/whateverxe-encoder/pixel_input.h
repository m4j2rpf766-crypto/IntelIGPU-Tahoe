#ifndef WEX_PIXEL_INPUT_H
#define WEX_PIXEL_INPUT_H

#include <CoreVideo/CoreVideo.h>
#include "wire.h"

/* Returns a +1 video-range NV12 buffer suitable for the Intel HEVC backend.
 * An already-compatible NV12 buffer is retained and returned unchanged.
 * BGRA is converted with vImage using the buffer's supported color primaries. */
OSStatus WEXCopyVideoRangeNV12(CVPixelBufferRef source,
                               CVPixelBufferRef *pixelBufferOut);

/* Encode standard CoreVideo color/chroma attachment strings as bounded wire
 * codepoints. UINT32_MAX means the source attachment was absent or unknown. */
OSStatus WEXPackVideoColorMetadata(CVPixelBufferRef source,
                                   WEXColorMetadata *metadata);

/* Recreate supported color and chroma attachments on the worker-side image. */
OSStatus WEXApplyVideoColorMetadata(CVPixelBufferRef destination,
                                    const WEXColorMetadata *metadata);

#endif
