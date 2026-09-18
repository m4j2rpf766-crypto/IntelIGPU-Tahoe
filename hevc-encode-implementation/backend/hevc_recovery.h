#pragma once
#include "gen12_scratch.h"
#include <mach/mach.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The private VA imports these wrappers. All ownership is acquired before
 * submission. An unknown lifetime/namespace is rejected without GPU work. */
void *reims_owned_resource_create(void *,const void *,size_t);
void reims_owned_resource_release(void *);
void reims_owned_context_release(void *);
int reims_owned_finish_fence(void *,unsigned);
kern_return_t reims_owned_vm_deallocate(vm_map_t,vm_address_t,vm_size_t);
int reims_context_error(void *);
/* 0 means rejected before submission, 2 means genuinely completed, -1 means
 * a native error was returned in *error. Scratch ownership is transferred
 * only after all references and a recovery slot have been acquired. */
int reims_execute_owned_batch(void *,void *,unsigned,unsigned,unsigned *,unsigned *,
                              ReimsScratch *,unsigned,const uint32_t *,size_t);
#ifdef __cplusplus
}
#endif
