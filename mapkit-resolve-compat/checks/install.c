// SPDX-License-Identifier: GPL-3.0-only
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

__attribute__((constructor)) static void install(void) {
    const char *path = getenv("REIMS_TEST_LIBRARY");
    void *handle = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    signed char (*fn)(void) = handle ? dlsym(handle, "ReimsInstallMapResolveCompat") : NULL;
    if (!fn || !fn()) {
        fputs("Map resolve installer preflight failed\n", stderr);
        abort();
    }
}
