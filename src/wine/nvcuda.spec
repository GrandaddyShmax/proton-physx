# nvcuda.dll (i386): the driver API subset PhysX uses (docs/FINDINGS.md). All __stdcall.
@ stdcall cuInit(long)
@ stdcall cuDriverGetVersion(ptr)
@ stdcall cuDeviceGet(ptr long)
@ stdcall cuDeviceGetCount(ptr)
@ stdcall cuDeviceGetName(ptr long long)
@ stdcall cuDeviceGetAttribute(ptr long long)
@ stdcall cuDeviceGetProperties(ptr long)
@ stdcall cuDeviceComputeCapability(ptr ptr long)
@ stdcall cuDeviceTotalMem(ptr long)
@ stdcall cuDeviceTotalMem_v2(ptr long) cuDeviceTotalMem
@ stdcall cuCtxCreate(ptr long long)
@ stdcall cuCtxCreate_v2(ptr long long) cuCtxCreate
@ stdcall cuCtxDestroy(long)
@ stdcall cuCtxDestroy_v2(long) cuCtxDestroy
@ stdcall cuCtxDetach(long) cuCtxDestroy
@ stdcall cuCtxPushCurrent(long)
@ stdcall cuCtxPushCurrent_v2(long) cuCtxPushCurrent
@ stdcall cuCtxPopCurrent(ptr)
@ stdcall cuCtxPopCurrent_v2(ptr) cuCtxPopCurrent
@ stdcall cuCtxGetCurrent(ptr)
@ stdcall cuCtxSetCurrent(long)
@ stdcall cuCtxSynchronize()
# APEX's own timing (Batman: Arkham Asylum). Its nvcuda imports are delay-loaded: a name missing
# here is not a failed load but an exception at the first call, which UE3 reports as a GPF.
@ stdcall cuEventCreate(ptr long)
@ stdcall cuEventCreate_v2(ptr long) cuEventCreate
@ stdcall cuEventDestroy(long)
@ stdcall cuEventDestroy_v2(long) cuEventDestroy
@ stdcall cuEventRecord(long long)
@ stdcall cuEventQuery(long)
@ stdcall cuEventSynchronize(long)
@ stdcall cuEventElapsedTime(ptr long long)
# Direct3D interop: APEX's framework imports these too (Arkham City, Borderlands 2, Alice). Refused.
@ stdcall cuGraphicsMapResources(long ptr long)
@ stdcall cuGraphicsUnmapResources(long ptr long)
@ stdcall cuGetExportTable(ptr ptr)
@ stdcall -private physxbridge_stats(ptr)
@ stdcall -private physxbridge_load()
