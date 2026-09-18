#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>

// 25G83 getter instructions return 64-bit sizes/offsets, 32-bit format/count.
// Nonvirtual exported methods. Never instantiate these private classes
// or depend on their C++ object size. The transaction owns the returned objects.
class IOSurface : public OSObject {
public:
 IOMemoryDescriptor*getMemoryDescriptor() const;
 uint64_t getClientAlignedOffset() const;
 uint64_t getPlaneOffset(uint32_t) const;
 uint64_t getPlaneBytesPerRow(uint32_t) const;
 uint64_t getWidth() const;
 uint64_t getHeight() const;
 uint32_t getPixelFormat() const;
 uint32_t getSurfaceID() const;
 uint32_t getPlaneCount() const;
};
class IOAccelResource2 : public OSObject {
public:
 IOAccelResource2*getStorageResource();
};
class IOAccelDisplayPipeTransaction2 : public OSObject {
public:
 uint64_t getTransactionDirtyBits() const;
 OSObject*getPlaneResource(unsigned,unsigned) const;
 IOSurface*getPlaneIOSurface(unsigned,unsigned) const;
};
