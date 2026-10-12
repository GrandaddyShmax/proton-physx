/* Prints what the driver offers. Build twice: 64-bit (the bridge's side) and 32-bit
 * (-m32, shows whether the driver accepts 32-bit CUDA at all; on RTX 50 it does not). */
#include <stdint.h>
#include "cudrv.h"

int main(void)
{
    int ver = 0;
    CU(cuDriverGetVersion, int *)(&ver);
    printf("process: %zu-bit, CUDA driver API %d.%d\n", sizeof(void *) * 8, ver / 1000, ver % 1000 / 10);

    CUresult r = CU(cuInit, unsigned)(0);
    if (r) {
        printf("cuInit -> %d%s\n", r, r == CU_ERROR_NO_DEVICE ? " (CUDA_ERROR_NO_DEVICE)" : "");
        return 3;
    }
    int n = 0;
    CU(cuDeviceGetCount, int *)(&n);
    for (int i = 0; i < n; i++) {
        CUdevice d;
        char name[128];
        size_t mem = 0;
        int maj = 0, min = 0, vmm = 0, timeout = 0, uva = 0;
        CU(cuDeviceGet, CUdevice *, int)(&d, i);
        CU(cuDeviceGetName, char *, int, CUdevice)(name, sizeof name, d);
        CU(cuDeviceTotalMem_v2, size_t *, CUdevice)(&mem, d);
        CU(cuDeviceGetAttribute, int *, int, CUdevice)(&maj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, d);
        CU(cuDeviceGetAttribute, int *, int, CUdevice)(&min, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, d);
        CU(cuDeviceGetAttribute, int *, int, CUdevice)(&vmm, CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED, d);
        CU(cuDeviceGetAttribute, int *, int, CUdevice)(&timeout, CU_DEVICE_ATTRIBUTE_KERNEL_EXEC_TIMEOUT, d);
        CU(cuDeviceGetAttribute, int *, int, CUdevice)(&uva, CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING, d);
        printf("device %d: %s, sm_%d%d, %zu MiB, VMM %s, unified addressing %s, kernel watchdog %s\n",
               i, name, maj, min, mem >> 20, vmm ? "yes" : "NO", uva ? "yes" : "no", timeout ? "on" : "off");
    }
    return 0;
}
