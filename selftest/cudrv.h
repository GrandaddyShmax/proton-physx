/* Minimal CUDA driver API declarations, resolved at runtime from libcuda.so.1.
 * No CUDA toolkit is needed on either OS: only the NVIDIA driver. */
#ifndef CUDRV_H
#define CUDRV_H

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext, *CUmodule, *CUfunction, *CUtexref;
typedef unsigned long long CUdeviceptr, CUmemGenericAllocationHandle;
typedef struct { int type, id; } CUmemLocation;
typedef struct {
    int type, requestedHandleTypes;
    CUmemLocation location;
    void *win32HandleMetaData;
    struct { unsigned char compressionType, gpuDirectRDMACapable; unsigned short usage; unsigned char reserved[4]; } allocFlags;
} CUmemAllocationProp;
typedef struct { CUmemLocation location; int flags; } CUmemAccessDesc;

enum {
    CU_DEVICE_ATTRIBUTE_KERNEL_EXEC_TIMEOUT = 17,
    CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING = 41,
    CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 75,
    CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = 76,
    CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED = 102,
    CU_JIT_INFO_LOG_BUFFER = 3,
    CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES = 4,
    CU_JIT_ERROR_LOG_BUFFER = 5,
    CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES = 6,
    CU_AD_FORMAT_FLOAT = 0x20,
    CU_ERROR_NO_DEVICE = 100,
};

static void *cudrv_lib;

static inline void *cudrv_sym(const char *name)
{
    if (!cudrv_lib && !(cudrv_lib = dlopen("libcuda.so.1", RTLD_NOW))) {
        printf("cannot load libcuda.so.1: %s\n", dlerror());
        exit(2);
    }
    void *p = dlsym(cudrv_lib, name);
    if (!p) {
        printf("libcuda.so.1 has no %s\n", name);
        exit(2);
    }
    return p;
}

/* CU(name, argtypes...)(args...) calls a driver entry point by name. */
#define CU(name, ...) ((CUresult (*)(__VA_ARGS__))cudrv_sym(#name))
#define CHECK(x) do { CUresult r_ = (x); if (r_) { printf("FAIL %s -> %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); exit(1); } } while (0)

static inline CUcontext cudrv_init_ctx(CUdevice *dev_out)
{
    CUdevice d;
    CUcontext c;
    CHECK(CU(cuInit, unsigned)(0));
    CHECK(CU(cuDeviceGet, CUdevice *, int)(&d, 0));
    CHECK(CU(cuDevicePrimaryCtxRetain, CUcontext *, CUdevice)(&c, d));
    CHECK(CU(cuCtxSetCurrent, CUcontext)(c));
    if (dev_out) *dev_out = d;
    return c;
}

#endif
