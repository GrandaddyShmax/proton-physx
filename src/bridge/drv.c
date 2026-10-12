/* Resolves the CUDA driver API from libcuda.so.1 (NVIDIA's 64-bit driver library, unmodified). */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

struct pxb_drv drv;
static void *lib;

CUresult drv_load(void)
{
    if (lib) return CUDA_SUCCESS;
    void *h = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        pxb_log(LOG_ERR, "cannot load libcuda.so.1: %s", dlerror());
        return CUDA_ERROR_NO_DEVICE;
    }
    int missing = 0;
#define PXB_X(name, params) \
    if (!(*(void **)&drv.name = dlsym(h, #name))) { \
        pxb_log(LOG_ERR, "libcuda.so.1 has no %s (driver too old?)", #name); \
        missing++; \
    }
    PXB_DRV_FUNCS(PXB_X)
#undef PXB_X
    if (missing) {
        memset(&drv, 0, sizeof drv);
        dlclose(h);
        return CUDA_ERROR_NO_DEVICE;
    }
    lib = h;
    return CUDA_SUCCESS;
}

const char *drv_error_name(CUresult r)
{
    const char *s = NULL;
    switch (r) { /* the ones the bridge returns itself, also before libcuda is loaded */
    case CUDA_SUCCESS: return "CUDA_SUCCESS";
    case CUDA_ERROR_INVALID_VALUE: return "CUDA_ERROR_INVALID_VALUE";
    case CUDA_ERROR_OUT_OF_MEMORY: return "CUDA_ERROR_OUT_OF_MEMORY";
    case CUDA_ERROR_NOT_INITIALIZED: return "CUDA_ERROR_NOT_INITIALIZED";
    case CUDA_ERROR_NO_DEVICE: return "CUDA_ERROR_NO_DEVICE";
    case CUDA_ERROR_INVALID_DEVICE: return "CUDA_ERROR_INVALID_DEVICE";
    case CUDA_ERROR_INVALID_CONTEXT: return "CUDA_ERROR_INVALID_CONTEXT";
    case CUDA_ERROR_NOT_SUPPORTED: return "CUDA_ERROR_NOT_SUPPORTED";
    }
    if (lib && drv.cuGetErrorName(r, &s) == CUDA_SUCCESS && s) return s;
    return "CUDA_ERROR_(unknown)";
}
