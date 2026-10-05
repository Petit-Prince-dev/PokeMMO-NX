#include "crash_recorder.h"
#include "diagnostics.h"
#include <switch.h>
#include <string.h>

// A dedicated exception stack large enough for fprintf: libnx runs the handler on it.
u8 __nx_exception_stack[0x8000] __attribute__((aligned(16)));
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static const CrashRegion *known_regions;
static unsigned known_region_count;

void crashRecorderSetRegions(const CrashRegion *regions, unsigned count) {
    known_regions = regions;
    known_region_count = regions ? count : 0;
}

static bool readableMemory(uintptr_t address, void *out, size_t bytes) {
    MemoryInfo info = {0};
    u32 page;
    if (!address || R_FAILED(svcQueryMemory(&info, &page, address))) return false;
    if (!(info.perm & Perm_R) || info.type == MemType_Unmapped || address < info.addr || address - info.addr > info.size ||
        bytes > info.size - (address - info.addr))
        return false;
    memcpy(out, (const void *)address, bytes);
    return true;
}

// Called by libnx in the faulting thread. Everything here avoids allocation. The report is written
// to the diagnostics log; the process then ends instead of showing the system error screen.
void __libnx_exception_handler(ThreadExceptionDump *dump) {
    CrashContext context = {.description = dump->error_desc,
                            .pstate = dump->pstate,
                            .esr = dump->esr,
                            .afsr0 = dump->afsr0,
                            .afsr1 = dump->afsr1,
                            .fp = dump->fp.x,
                            .lr = dump->lr.x,
                            .sp = dump->sp.x,
                            .pc = dump->pc.x,
                            .far = dump->far.x};
    for (unsigned i = 0; i < 29; ++i) context.x[i] = dump->cpu_gprs[i].x;
    FILE *log = diagnosticsLog();
    if (log) {
        fprintf(log, "crash.begin aarch64=%u\n", threadExceptionIsAArch64(dump));
        crashFormat(log, &context, known_regions, known_region_count, readableMemory);
        fflush(log);
    }
    svcExitProcess();
}
