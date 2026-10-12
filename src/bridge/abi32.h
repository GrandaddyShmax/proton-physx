/* CUDA 6.5 runtime types as a 32-bit Windows program sees them (size_t and pointers are 4 bytes).
 * Shared by the unix-side bridge and the i386 PE side (phase 3), so: fixed-width types only.
 * Layouts checked against NVIDIA's cudart32_65.dll (docs/FINDINGS.md, 2026-10-10). */
#ifndef PXB_ABI32_H
#define PXB_ABI32_H

#include <stdint.h>

/* cudaError_t: X(name, value, description); the descriptions are the ones cudart32_65.dll returns */
#define PXB_CUDART_ERRORS(X) \
    X(cudaSuccess, 0, "no error") \
    X(cudaErrorMissingConfiguration, 1, "__global__ function call is not configured") \
    X(cudaErrorMemoryAllocation, 2, "out of memory") \
    X(cudaErrorInitializationError, 3, "initialization error") \
    X(cudaErrorLaunchFailure, 4, "unspecified launch failure") \
    X(cudaErrorPriorLaunchFailure, 5, "unspecified launch failure in prior launch") \
    X(cudaErrorLaunchTimeout, 6, "the launch timed out and was terminated") \
    X(cudaErrorLaunchOutOfResources, 7, "too many resources requested for launch") \
    X(cudaErrorInvalidDeviceFunction, 8, "invalid device function ") \
    X(cudaErrorInvalidConfiguration, 9, "invalid configuration argument") \
    X(cudaErrorInvalidDevice, 10, "invalid device ordinal") \
    X(cudaErrorInvalidValue, 11, "invalid argument") \
    X(cudaErrorInvalidPitchValue, 12, "invalid pitch argument") \
    X(cudaErrorInvalidSymbol, 13, "invalid device symbol") \
    X(cudaErrorMapBufferObjectFailed, 14, "mapping of buffer object failed") \
    X(cudaErrorUnmapBufferObjectFailed, 15, "unmapping of buffer object failed") \
    X(cudaErrorInvalidHostPointer, 16, "invalid host pointer") \
    X(cudaErrorInvalidDevicePointer, 17, "invalid device pointer") \
    X(cudaErrorInvalidTexture, 18, "invalid texture reference") \
    X(cudaErrorInvalidTextureBinding, 19, "texture is not bound to a pointer") \
    X(cudaErrorInvalidChannelDescriptor, 20, "invalid channel descriptor") \
    X(cudaErrorInvalidMemcpyDirection, 21, "invalid copy direction for memcpy") \
    X(cudaErrorAddressOfConstant, 22, "invalid address of constant") \
    X(cudaErrorTextureFetchFailed, 23, "fetch from texture failed") \
    X(cudaErrorTextureNotBound, 24, "cannot fetch from a texture that is not bound") \
    X(cudaErrorSynchronizationError, 25, "incorrect use of __syncthreads()") \
    X(cudaErrorInvalidFilterSetting, 26, "linear filtering not supported for non-float type") \
    X(cudaErrorInvalidNormSetting, 27, "read as normalized float not supported for 32-bit non float type") \
    X(cudaErrorMixedDeviceExecution, 28, "device emulation mode and device execution mode cannot be mixed") \
    X(cudaErrorCudartUnloading, 29, "driver shutting down") \
    X(cudaErrorUnknown, 30, "unknown error") \
    X(cudaErrorNotYetImplemented, 31, "feature not yet implemented") \
    X(cudaErrorMemoryValueTooLarge, 32, "memory size or pointer value too large to fit in 32 bit") \
    X(cudaErrorInvalidResourceHandle, 33, "invalid resource handle") \
    X(cudaErrorNotReady, 34, "device not ready") \
    X(cudaErrorInsufficientDriver, 35, "CUDA driver version is insufficient for CUDA runtime version") \
    X(cudaErrorSetOnActiveProcess, 36, "cannot set while device is active in this process") \
    X(cudaErrorInvalidSurface, 37, "invalid surface reference") \
    X(cudaErrorNoDevice, 38, "no CUDA-capable device is detected") \
    X(cudaErrorECCUncorrectable, 39, "uncorrectable ECC error encountered") \
    X(cudaErrorSharedObjectSymbolNotFound, 40, "shared object symbol not found") \
    X(cudaErrorSharedObjectInitFailed, 41, "shared object initialization failed") \
    X(cudaErrorUnsupportedLimit, 42, "limit is not supported on this architecture") \
    X(cudaErrorDuplicateVariableName, 43, "duplicate global variable looked up by string name") \
    X(cudaErrorDuplicateTextureName, 44, "duplicate texture looked up by string name") \
    X(cudaErrorDuplicateSurfaceName, 45, "duplicate surface looked up by string name") \
    X(cudaErrorDevicesUnavailable, 46, "all CUDA-capable devices are busy or unavailable") \
    X(cudaErrorInvalidKernelImage, 47, "device kernel image is invalid") \
    X(cudaErrorNoKernelImageForDevice, 48, "no kernel image is available for execution on the device") \
    X(cudaErrorIncompatibleDriverContext, 49, "incompatible driver context") \
    X(cudaErrorPeerAccessAlreadyEnabled, 50, "peer access is already enabled") \
    X(cudaErrorPeerAccessNotEnabled, 51, "peer access has not been enabled") \
    X(cudaErrorDeviceAlreadyInUse, 54, "exclusive-thread device already in use by a different thread") \
    X(cudaErrorProfilerDisabled, 55, "profiler disabled while using external profiling tool") \
    X(cudaErrorProfilerNotInitialized, 56, "profiler not initialized") \
    X(cudaErrorProfilerAlreadyStarted, 57, "profiler already started") \
    X(cudaErrorProfilerAlreadyStopped, 58, "profiler already stopped") \
    X(cudaErrorAssert, 59, "device-side assert triggered") \
    X(cudaErrorTooManyPeers, 60, "peer mapping resources exhausted") \
    X(cudaErrorHostMemoryAlreadyRegistered, 61, "part or all of the requested memory range is already mapped") \
    X(cudaErrorHostMemoryNotRegistered, 62, "pointer does not correspond to a registered memory region") \
    X(cudaErrorOperatingSystem, 63, "OS call failed or operation not supported on this OS") \
    X(cudaErrorPeerAccessUnsupported, 64, "peer access is not supported between these two devices") \
    X(cudaErrorLaunchMaxDepthExceeded, 65, "launch would exceed maximum depth of nested launches") \
    X(cudaErrorLaunchFileScopedTex, 66, "launch failed because kernel uses unsupported, file-scoped textures (texture objects are supported)") \
    X(cudaErrorLaunchFileScopedSurf, 67, "launch failed because kernel uses unsupported, file-scoped surfaces (surface objects are supported)") \
    X(cudaErrorSyncDepthExceeded, 68, "cudaDeviceSynchronize failed because caller's grid depth exceeds cudaLimitDevRuntimeSyncDepth") \
    X(cudaErrorLaunchPendingCountExceeded, 69, "launch failed because launch would exceed cudaLimitDevRuntimePendingLaunchCount") \
    X(cudaErrorNotPermitted, 70, "operation not permitted") \
    X(cudaErrorNotSupported, 71, "operation not supported") \
    X(cudaErrorHardwareStackError, 72, "hardware stack error") \
    X(cudaErrorIllegalInstruction, 73, "an illegal instruction was encountered") \
    X(cudaErrorMisalignedAddress, 74, "misaligned address") \
    X(cudaErrorInvalidAddressSpace, 75, "operation not supported on global/shared address space") \
    X(cudaErrorInvalidPc, 76, "invalid program counter") \
    X(cudaErrorIllegalAddress, 77, "an illegal memory access was encountered") \
    X(cudaErrorInvalidPtx, 78, "a PTX JIT compilation failed") \
    X(cudaErrorInvalidGraphicsContext, 79, "invalid OpenGL or DirectX context") \
    X(cudaErrorStartupFailure, 127, "startup failure in cuda runtime")

enum pxb_cudart_error {
#define PXB_X(name, value, desc) name = value,
    PXB_CUDART_ERRORS(PXB_X)
#undef PXB_X
};

enum { /* cudaMemcpyKind */
    cudaMemcpyHostToHost = 0,
    cudaMemcpyHostToDevice = 1,
    cudaMemcpyDeviceToHost = 2,
    cudaMemcpyDeviceToDevice = 3,
    cudaMemcpyDefault = 4, /* needs unified addressing, which 32-bit CUDA never had */
};

enum { /* cudaChannelFormatKind */
    cudaChannelFormatKindSigned = 0,
    cudaChannelFormatKindUnsigned = 1,
    cudaChannelFormatKindFloat = 2,
    cudaChannelFormatKindNone = 3,
};

typedef struct { uint32_t x, y, z; } pxb_dim3;

typedef struct { int32_t x, y, z, w, f; } pxb_channel_desc32; /* cudaChannelFormatDesc */

typedef struct { /* textureReference: the host-side object a texture<> variable is */
    int32_t normalized;
    int32_t filterMode;
    int32_t addressMode[3];
    pxb_channel_desc32 channelDesc;
    int32_t sRGB;
    uint32_t maxAnisotropy;
    int32_t mipmapFilterMode;
    float mipmapLevelBias, minMipmapLevelClamp, maxMipmapLevelClamp;
    int32_t cudaReserved[15];
} pxb_texref32;

typedef struct { /* cudaDeviceProp, CUDA 6.5 */
    char name[256];
    uint32_t totalGlobalMem;
    uint32_t sharedMemPerBlock;
    int32_t regsPerBlock;
    int32_t warpSize;
    uint32_t memPitch;
    int32_t maxThreadsPerBlock;
    int32_t maxThreadsDim[3];
    int32_t maxGridSize[3];
    int32_t clockRate;
    uint32_t totalConstMem;
    int32_t major;
    int32_t minor;
    uint32_t textureAlignment;
    uint32_t texturePitchAlignment;
    int32_t deviceOverlap;
    int32_t multiProcessorCount;
    int32_t kernelExecTimeoutEnabled;
    int32_t integrated;
    int32_t canMapHostMemory;
    int32_t computeMode;
    int32_t maxTexture1D;
    int32_t maxTexture1DMipmap;
    int32_t maxTexture1DLinear;
    int32_t maxTexture2D[2];
    int32_t maxTexture2DMipmap[2];
    int32_t maxTexture2DLinear[3];
    int32_t maxTexture2DGather[2];
    int32_t maxTexture3D[3];
    int32_t maxTexture3DAlt[3];
    int32_t maxTextureCubemap;
    int32_t maxTexture1DLayered[2];
    int32_t maxTexture2DLayered[3];
    int32_t maxTextureCubemapLayered[2];
    int32_t maxSurface1D;
    int32_t maxSurface2D[2];
    int32_t maxSurface3D[3];
    int32_t maxSurface1DLayered[2];
    int32_t maxSurface2DLayered[3];
    int32_t maxSurfaceCubemap;
    int32_t maxSurfaceCubemapLayered[2];
    uint32_t surfaceAlignment;
    int32_t concurrentKernels;
    int32_t ECCEnabled;
    int32_t pciBusID;
    int32_t pciDeviceID;
    int32_t pciDomainID;
    int32_t tccDriver;
    int32_t asyncEngineCount;
    int32_t unifiedAddressing;
    int32_t memoryClockRate;
    int32_t memoryBusWidth;
    int32_t l2CacheSize;
    int32_t maxThreadsPerMultiProcessor;
    int32_t streamPrioritiesSupported;
    int32_t globalL1CacheSupported;
    int32_t localL1CacheSupported;
    uint32_t sharedMemPerMultiprocessor;
    int32_t regsPerMultiprocessor;
    int32_t managedMemory;
    int32_t isMultiGpuBoard;
    int32_t multiGpuBoardGroupID;
} pxb_device_prop32;

typedef struct { /* CUdevprop (driver API, cuDeviceGetProperties) */
    int32_t maxThreadsPerBlock;
    int32_t maxThreadsDim[3];
    int32_t maxGridSize[3];
    int32_t sharedMemPerBlock;
    int32_t totalConstantMemory;
    int32_t SIMDWidth;
    int32_t memPitch;
    int32_t regsPerBlock;
    int32_t clockRate;
    int32_t textureAlign;
} pxb_devprop32;

/* what nvcc 6.5 passes to __cudaRegisterFatBinary (the .nvFatBi section holds an array of these) */
#define PXB_FATBIN_WRAPPER_MAGIC 0x466243b1u
typedef struct { uint32_t magic, version, fatbin, filename_or_fatbins; } pxb_fatbin_wrapper32;

/* compile-time layout checks (C99: a negative array size fails the build) */
typedef char pxb_check_device_prop32[sizeof(pxb_device_prop32) == 596 ? 1 : -1];
typedef char pxb_check_devprop32[sizeof(pxb_devprop32) == 56 ? 1 : -1];
typedef char pxb_check_texref32[sizeof(pxb_texref32) == 124 ? 1 : -1];

#endif
