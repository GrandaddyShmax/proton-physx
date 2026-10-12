/* The CUDA driver API as the bridge uses it: types, constants, and a table of entry points that
 * drv_load() resolves from libcuda.so.1 at run time. No CUDA toolkit needed on either OS. */
#ifndef PXB_DRV_H
#define PXB_DRV_H

#include <stddef.h>
#include <stdint.h>

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef struct CUtexref_st *CUtexref;
typedef struct CUstream_st *CUstream;
typedef struct CUevent_st *CUevent;
typedef unsigned long long CUdeviceptr;
typedef unsigned long long CUmemGenericAllocationHandle;
typedef struct { int type, id; } CUmemLocation;
typedef struct {
    int type, requestedHandleTypes;
    CUmemLocation location;
    void *win32HandleMetaData;
    struct { unsigned char compressionType, gpuDirectRDMACapable; unsigned short usage; unsigned char reserved[4]; } allocFlags;
} CUmemAllocationProp;
typedef struct { CUmemLocation location; int flags; } CUmemAccessDesc;

enum {
    CUDA_SUCCESS = 0,
    CUDA_ERROR_INVALID_VALUE = 1,
    CUDA_ERROR_OUT_OF_MEMORY = 2,
    CUDA_ERROR_NOT_INITIALIZED = 3,
    CUDA_ERROR_DEINITIALIZED = 4,
    CUDA_ERROR_NO_DEVICE = 100,
    CUDA_ERROR_INVALID_DEVICE = 101,
    CUDA_ERROR_INVALID_IMAGE = 200,
    CUDA_ERROR_INVALID_CONTEXT = 201,
    CUDA_ERROR_NO_BINARY_FOR_GPU = 209,
    CUDA_ERROR_ECC_UNCORRECTABLE = 214,
    CUDA_ERROR_INVALID_PTX = 218,
    CUDA_ERROR_UNSUPPORTED_PTX_VERSION = 222,
    CUDA_ERROR_INVALID_HANDLE = 400,
    CUDA_ERROR_NOT_FOUND = 500,
    CUDA_ERROR_NOT_READY = 600,
    CUDA_ERROR_ILLEGAL_ADDRESS = 700,
    CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES = 701,
    CUDA_ERROR_LAUNCH_TIMEOUT = 702,
    CUDA_ERROR_ASSERT = 710,
    CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED = 712,
    CUDA_ERROR_HOST_MEMORY_NOT_REGISTERED = 713,
    CUDA_ERROR_HARDWARE_STACK_ERROR = 714,
    CUDA_ERROR_ILLEGAL_INSTRUCTION = 715,
    CUDA_ERROR_MISALIGNED_ADDRESS = 716,
    CUDA_ERROR_INVALID_ADDRESS_SPACE = 717,
    CUDA_ERROR_INVALID_PC = 718,
    CUDA_ERROR_LAUNCH_FAILED = 719,
    CUDA_ERROR_NOT_PERMITTED = 800,
    CUDA_ERROR_NOT_SUPPORTED = 801,
    CUDA_ERROR_UNKNOWN = 999,
};

enum {
    CU_DEVICE_ATTRIBUTE_PCI_BUS_ID = 33,
    CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID = 34,
    CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING = 41,
    CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID = 50,
    CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY = 83,
    CU_MEM_ALLOCATION_TYPE_PINNED = 1,
    CU_MEM_LOCATION_TYPE_DEVICE = 1,
    CU_MEM_ACCESS_FLAGS_PROT_READWRITE = 3,
    CU_MEM_ALLOC_GRANULARITY_MINIMUM = 0,
    CU_JIT_WALL_TIME = 2,
    CU_JIT_INFO_LOG_BUFFER = 3,
    CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES = 4,
    CU_JIT_ERROR_LOG_BUFFER = 5,
    CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES = 6,
    CU_TRSF_READ_AS_INTEGER = 0x01,
    CU_TRSF_NORMALIZED_COORDINATES = 0x02,
    CU_TRSF_SRGB = 0x10,
    CU_AD_FORMAT_UNSIGNED_INT8 = 0x01,
    CU_AD_FORMAT_UNSIGNED_INT16 = 0x02,
    CU_AD_FORMAT_UNSIGNED_INT32 = 0x03,
    CU_AD_FORMAT_SIGNED_INT8 = 0x08,
    CU_AD_FORMAT_SIGNED_INT16 = 0x09,
    CU_AD_FORMAT_SIGNED_INT32 = 0x0a,
    CU_AD_FORMAT_HALF = 0x10,
    CU_AD_FORMAT_FLOAT = 0x20,
    CU_CTX_SCHED_MASK = 0x07,
};

#define CU_LAUNCH_PARAM_END ((void *)0)
#define CU_LAUNCH_PARAM_BUFFER_POINTER ((void *)1)
#define CU_LAUNCH_PARAM_BUFFER_SIZE ((void *)2)

/* X(name, parameter list): every driver entry point the bridge calls */
#define PXB_DRV_FUNCS(X) \
    X(cuInit, (unsigned flags)) \
    X(cuDriverGetVersion, (int *version)) \
    X(cuGetErrorName, (CUresult err, const char **name)) \
    X(cuDeviceGet, (CUdevice *dev, int ordinal)) \
    X(cuDeviceGetCount, (int *count)) \
    X(cuDeviceGetName, (char *name, int len, CUdevice dev)) \
    X(cuDeviceGetAttribute, (int *value, int attr, CUdevice dev)) \
    X(cuDeviceTotalMem_v2, (size_t *bytes, CUdevice dev)) \
    X(cuDevicePrimaryCtxRetain, (CUcontext *ctx, CUdevice dev)) \
    X(cuDevicePrimaryCtxRelease_v2, (CUdevice dev)) \
    X(cuDevicePrimaryCtxSetFlags_v2, (CUdevice dev, unsigned flags)) \
    X(cuCtxSetCurrent, (CUcontext ctx)) \
    X(cuCtxSynchronize, (void)) \
    X(cuCtxSetCacheConfig, (int config)) \
    X(cuMemGetInfo_v2, (size_t *free_bytes, size_t *total_bytes)) \
    X(cuMemAddressReserve, (CUdeviceptr *ptr, size_t size, size_t align, CUdeviceptr addr, unsigned long long flags)) \
    X(cuMemAddressFree, (CUdeviceptr ptr, size_t size)) \
    X(cuMemGetAllocationGranularity, (size_t *gran, const CUmemAllocationProp *prop, int option)) \
    X(cuMemCreate, (CUmemGenericAllocationHandle *h, size_t size, const CUmemAllocationProp *prop, unsigned long long flags)) \
    X(cuMemRelease, (CUmemGenericAllocationHandle h)) \
    X(cuMemMap, (CUdeviceptr ptr, size_t size, size_t offset, CUmemGenericAllocationHandle h, unsigned long long flags)) \
    X(cuMemUnmap, (CUdeviceptr ptr, size_t size)) \
    X(cuMemSetAccess, (CUdeviceptr ptr, size_t size, const CUmemAccessDesc *desc, size_t count)) \
    X(cuMemcpyHtoD_v2, (CUdeviceptr dst, const void *src, size_t n)) \
    X(cuMemcpyDtoH_v2, (void *dst, CUdeviceptr src, size_t n)) \
    X(cuMemcpyDtoD_v2, (CUdeviceptr dst, CUdeviceptr src, size_t n)) \
    X(cuMemcpyHtoDAsync_v2, (CUdeviceptr dst, const void *src, size_t n, CUstream s)) \
    X(cuMemcpyDtoHAsync_v2, (void *dst, CUdeviceptr src, size_t n, CUstream s)) \
    X(cuMemcpyDtoDAsync_v2, (CUdeviceptr dst, CUdeviceptr src, size_t n, CUstream s)) \
    X(cuMemsetD8_v2, (CUdeviceptr dst, unsigned char value, size_t n)) \
    X(cuMemHostRegister_v2, (void *p, size_t size, unsigned flags)) \
    X(cuMemHostUnregister, (void *p)) \
    X(cuModuleLoadDataEx, (CUmodule *mod, const void *image, unsigned nopts, int *opts, void **vals)) \
    X(cuModuleUnload, (CUmodule mod)) \
    X(cuModuleGetFunction, (CUfunction *fn, CUmodule mod, const char *name)) \
    X(cuModuleGetGlobal_v2, (CUdeviceptr *ptr, size_t *size, CUmodule mod, const char *name)) \
    X(cuModuleGetTexRef, (CUtexref *tex, CUmodule mod, const char *name)) \
    X(cuTexRefSetAddress_v2, (size_t *offset, CUtexref tex, CUdeviceptr ptr, size_t size)) \
    X(cuTexRefSetFormat, (CUtexref tex, int format, int channels)) \
    X(cuTexRefSetFlags, (CUtexref tex, unsigned flags)) \
    X(cuTexRefSetFilterMode, (CUtexref tex, int mode)) \
    X(cuLaunchKernel, (CUfunction fn, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by, unsigned bz, \
                       unsigned shmem, CUstream s, void **params, void **extra)) \
    X(cuStreamCreate, (CUstream *s, unsigned flags)) \
    X(cuStreamDestroy_v2, (CUstream s)) \
    X(cuStreamSynchronize, (CUstream s)) \
    X(cuStreamQuery, (CUstream s)) \
    X(cuEventCreate, (CUevent *ev, unsigned flags)) \
    X(cuEventDestroy_v2, (CUevent ev)) \
    X(cuEventRecord, (CUevent ev, CUstream s)) \
    X(cuEventQuery, (CUevent ev)) \
    X(cuEventSynchronize, (CUevent ev)) \
    X(cuEventElapsedTime, (float *ms, CUevent start, CUevent end))

struct pxb_drv {
#define PXB_X(name, params) CUresult (*name) params;
    PXB_DRV_FUNCS(PXB_X)
#undef PXB_X
};

extern struct pxb_drv drv;

/* Loads libcuda.so.1 and resolves every entry point (once; it stays loaded). Returns CUDA_SUCCESS or an error. */
CUresult drv_load(void);
const char *drv_error_name(CUresult r);

#endif
