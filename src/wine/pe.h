/* Shared by the i386 PE sides (cudart32_65.dll, nvcuda.dll): Wine headers, the unix library
 * handshake and the call helper. Define PXB_UNAVAILABLE (what every call returns without the
 * unix side) before including this. */
#ifndef PXB_PE_H
#define PXB_PE_H

#include <stdarg.h>
#include <stdint.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "../bridge/pxb.h"
#include "thunks.h"

/* The game's pointer as the 32-bit word the unix side zero-extends. */
#define U(x) ((uint32_t)(ULONG_PTR)(x))
#define CALL(name, ...) pe_call(pxb_call_##name, (const uint32_t[]){ __VA_ARGS__ })
#define CALL0(name) pe_call(pxb_call_##name, NULL)

static BOOL unix_ok;

static inline int pe_call(enum pxb_call code, const uint32_t *args)
{
    return unix_ok ? (int)WINE_UNIX_CALL(code, (void *)args) : PXB_UNAVAILABLE;
}

/* DLL_PROCESS_ATTACH. The unix library is 64-bit, so it only loads when the 32-bit program runs
 * in a 64-bit process (Wine WoW64, PROTON_USE_WOW64=1). Otherwise every call fails cleanly and
 * PhysX uses the CPU, as it does without CUDA. */
static inline void pe_init(HINSTANCE inst, const char *dll)
{
    char exe[MAX_PATH];
    DisableThreadLibraryCalls(inst);
    unix_ok = !__wine_init_unix_call();
    if (unix_ok && GetModuleFileNameA(NULL, exe, sizeof exe) < sizeof exe)
        CALL(set_process_name, U(exe)); /* names the log after the game */
    if (!unix_ok) {
        __wine_dbg_output("physx-bridge: ");
        __wine_dbg_output(dll);
        __wine_dbg_output(": no unix library, CUDA unavailable (needs a 64-bit Wine process: PROTON_USE_WOW64=1)\n");
    }
}

#endif
