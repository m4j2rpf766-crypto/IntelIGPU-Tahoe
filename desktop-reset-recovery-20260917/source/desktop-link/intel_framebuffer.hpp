#pragma once
#include <IOKit/graphics/IOFramebuffer.h>
// Explicit runtime handoff. The caller retains the returned framebuffer.
IOFramebuffer *ReimsIntelFramebufferCreate(IOService *accelerator, IOFramebuffer *boot);
