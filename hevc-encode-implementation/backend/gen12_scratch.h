#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *resource;
    uintptr_t backing;
    size_t bytes;
    uint32_t handle;
} ReimsScratch;
// IOAccelShared must belong to the same device/shared namespace as the batch.
// Caller retains each allocation until actual GPU completion. Destruction is
// only valid when no submitted batch still refers to this resource.
int reims_scratch_create(void *shared,size_t requested,ReimsScratch *out);
void reims_scratch_destroy(ReimsScratch *resource);
#ifdef __cplusplus
}
#endif
