#include "game.h"
#include "crash_recorder.h"
#include "diagnostics.h"
#include "display.h"
#include "elf_executable.h"
#include "linux_audio.h"
#include "linux_directories.h"
#include "linux_dl.h"
#include "linux_files.h"
#include "linux_gtk.h"
#include "linux_net.h"
#include "linux_runtime.h"
#include "linux_sdl.h"
#include "linux_semaphore.h"
#include "linux_stdio.h"
#include "linux_sync.h"
#include "linux_threads.h"
#include "linux_tls.h"
#include "linux_trap.h"
#include "linux_vm.h"
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

// The client sees this folder as "/": its install is /game, its temporary files /tmp, its home /home/switch.
#define ROOT_DIR "sdmc:/switch/PokeMMO/isolate-root"
#define STACK_BYTES (16u * 1024u * 1024u)
#define LINE_BYTES 240u
#define CLOSE_GRACE_MS 8000u  // after the system asks the application to close, the client gets this long to finish before it is left behind

typedef int (*MainFunction)(int argc, char **argv);
static struct {
    uintptr_t main_entry;
    _Atomic bool returned;
    int rc;
} run;
static ElfExecutable mapped;
static LinuxPthread worker;
static bool abandoned;
static const char *failure = "";
static LinuxRuntimeExport exports[ELF_EXPORT_MAX];
static CrashRegion crash_regions[1];

// ---- the client's stdout/stderr: whole lines go to the diagnostics file, nothing is kept otherwise -------------------------------
static char pending[3][LINE_BYTES + 1];
static size_t pending_bytes[3];
static atomic_flag capture_lock = ATOMIC_FLAG_INIT;
static void emitLine(int fd) {
    diagnosticsTrace("client.console fd=%d text=%.*s", fd, (int)pending_bytes[fd], pending[fd]);
    pending_bytes[fd] = 0;
}
static int64_t sink(void *context, int fd, const void *bytes, size_t count, int *error) {
    (void)context;
    if (fd < 1 || fd > 2) {
        *error = LINUX_EBADF;
        return -1;
    }
    if (!diagnosticsLog()) return (int64_t)count;
    while (atomic_flag_test_and_set_explicit(&capture_lock, memory_order_acquire)) linuxThreadYield();
    const unsigned char *p = bytes;
    for (size_t i = 0; i < count; ++i) {
        if (p[i] == '\n' || pending_bytes[fd] == LINE_BYTES) {
            emitLine(fd);
            if (p[i] == '\n') continue;
        }
        pending[fd][pending_bytes[fd]++] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
    }
    atomic_flag_clear_explicit(&capture_lock, memory_order_release);
    return (int64_t)count;
}
static void flushCapture(void) {
    if (!diagnosticsLog()) return;
    while (atomic_flag_test_and_set_explicit(&capture_lock, memory_order_acquire)) linuxThreadYield();
    for (int fd = 1; fd <= 2; ++fd)
        if (pending_bytes[fd]) emitLine(fd);
    atomic_flag_clear_explicit(&capture_lock, memory_order_release);
}

extern char *fake_heap_end;  // libnx: the end of the heap it gave the application
static uint64_t milliseconds(void) { return armTicksToNs(armGetSystemTick()) / 1000000u; }

static void logState(const char *when) {
    if (!diagnosticsLog()) return;
    LinuxVmStats v = linuxVmStats();
    LinuxThreadStats t = linuxThreadsStats();
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    // What the application can still allocate: the heap libnx gave it is taken whole at start, so "used" above says nothing.
    unsigned long long heap_free = (unsigned long long)((uintptr_t)fake_heap_end - (uintptr_t)sbrk(0)) + (unsigned long long)mallinfo().fordblks;
    diagnosticsTrace(
        "game.state when=%s vm_arenas=%zu vm_active_pages=%zu vm_mapped_pages=%zu vm_backing_bytes=%zu threads=%zu live=%zu created=%zu heap_free_mib=%llu process_used_mib=%llu total_mib=%llu",
        when, v.arenas, v.active_pages, v.mapped_pages, v.backing_bytes, t.threads, t.live_threads, t.created, heap_free >> 20,
        (unsigned long long)(used >> 20), (unsigned long long)(total >> 20));
}

// ---- watchdog -----------------------------------------------------------------------------------------------------------------
// The wait loop below must stay alive whatever the game does, so it takes no lock and beats a counter. Only with the diagnostics file on, this
// thread writes where every game thread is when the beat stops (a deadlock) and the state of the memory once in a while.
static atomic_ullong wait_heartbeat;
static atomic_bool watchdog_stop;
static atomic_ullong watchdog_begin_ms;
static Thread watchdog_thread;
static bool watchdog_running;
static bool readableMemory(uintptr_t address, void *out, size_t bytes) {
    MemoryInfo info = {0};
    u32 page;
    if (R_FAILED(svcQueryMemory(&info, &page, address)) || !(info.perm & Perm_R) || info.addr > address || address - info.addr + bytes > info.size)
        return false;
    memcpy(out, (const void *)address, bytes);
    return true;
}
static void dumpGuestThreads(unsigned seconds) {
    FILE *log = diagnosticsLog();
    if (!log) return;
    flockfile(log);  // the dump stays in one piece
    fprintf(log, "watchdog.stall seconds=%u\n", seconds);
    CrashRegion regions[40];
    unsigned region_count = 0;
    regions[region_count++] = (CrashRegion){"client", (uintptr_t)mapped.base, mapped.layout.span};
    LinuxDlRegion libraries[24];
    unsigned library_count = linuxDlRegions(libraries, 24);
    for (unsigned i = 0; i < library_count && region_count < 40; ++i)
        regions[region_count++] = (CrashRegion){libraries[i].name, libraries[i].base, libraries[i].span};
    LinuxThreadSnapshot threads[LINUX_THREAD_MAX_THREADS];
    unsigned count = linuxThreadsSnapshot(threads, LINUX_THREAD_MAX_THREADS);
    for (unsigned i = 0; i < count; ++i) {
        if (!threads[i].handle) continue;
        ThreadContext context;
        Result paused = svcSetThreadActivity(threads[i].handle, ThreadActivity_Paused);
        Result got = R_SUCCEEDED(paused) ? svcGetThreadContext3(&context, threads[i].handle) : paused;
        if (R_SUCCEEDED(paused)) svcSetThreadActivity(threads[i].handle, ThreadActivity_Runnable);
        fprintf(log, "watchdog.thread id=%llu handle=0x%x pause_rc=0x%x context_rc=0x%x\n", (unsigned long long)threads[i].id, threads[i].handle, paused, got);
        if (R_FAILED(got)) continue;
        CrashContext crash = {0};
        for (unsigned r = 0; r < 29; ++r) crash.x[r] = context.cpu_gprs[r].x;
        crash.fp = context.fp;
        crash.lr = context.lr;
        crash.sp = context.sp;
        crash.pc = context.pc.x;
        crash.pstate = context.psr;
        crashFormat(log, &crash, regions, region_count, readableMemory);
        fflush(log);
    }
    fprintf(log, "watchdog.end threads=%u\n", count);
    fflush(log);
    funlockfile(log);
}
static void watchdogMain(void *argument) {
    (void)argument;
    unsigned long long seen = 0;
    uint64_t changed = milliseconds();
    unsigned seconds = 0, dumps = 0;
    bool dumped_for_this_stall = false;
    while (!atomic_load(&watchdog_stop)) {
        svcSleepThread(1000ull * 1000000ull);
        ++seconds;
        unsigned long long beat = atomic_load(&wait_heartbeat);
        if (beat != seen) {
            seen = beat;
            changed = milliseconds();
            dumped_for_this_stall = false;
        } else if (milliseconds() - changed > 4000u && !dumped_for_this_stall && dumps < 4) {
            dumped_for_this_stall = true;
            ++dumps;
            dumpGuestThreads(seconds);
        }
        if (seconds <= 60 ? seconds % 5 == 0 : seconds % 60 == 0) {
            diagnosticsTrace("game.wait elapsed_ms=%llu heartbeat=%llu", (unsigned long long)(milliseconds() - atomic_load(&watchdog_begin_ms)), beat);
            logState("waiting");
        }
    }
}
static void startWatchdog(void) {
    if (!diagnosticsLog()) return;
    atomic_store(&wait_heartbeat, 0);
    atomic_store(&watchdog_stop, false);
    atomic_store(&watchdog_begin_ms, milliseconds());
    // More urgent than the game's threads, so that it still runs when they spin; a plainer priority if that is refused.
    bool created = R_SUCCEEDED(threadCreate(&watchdog_thread, watchdogMain, NULL, NULL, 0x10000, 0x24, linuxThreadBackgroundCore())) ||
                   R_SUCCEEDED(threadCreate(&watchdog_thread, watchdogMain, NULL, NULL, 0x10000, 0x2c, linuxThreadBackgroundCore()));
    if (created && R_SUCCEEDED(threadStart(&watchdog_thread))) watchdog_running = true;
}
static void stopWatchdog(void) {
    if (!watchdog_running) return;
    atomic_store(&watchdog_stop, true);
    threadWaitForExit(&watchdog_thread);
    threadClose(&watchdog_thread);
    watchdog_running = false;
}

// ---- diagnostics ----------------------------------------------------------------------------------------------------------------
// Every import the client really called, with its first caller as an offset in the image, then the totals of the adapters.
static void summary(void) {
    if (!diagnosticsLog()) return;
    LinuxTrapStats trap_stats = linuxTrapStats();
    for (unsigned i = 0; i < trap_stats.count; ++i) {
        unsigned n = linuxTrapCalls(i);
        if (!n) continue;
        uintptr_t caller = linuxTrapFirstCaller(i);
        diagnosticsTrace("game.trap.called name=%s calls=%u first_caller=0x%llx client_offset=0x%llx", linuxTrapName(i), n, (unsigned long long)caller,
                         (unsigned long long)(caller - (uintptr_t)mapped.base));
    }
    LinuxSyscallRecord syscalls[8];
    size_t unsupported = linuxRuntimeUnsupportedSyscalls(syscalls, 8);
    for (size_t i = 0; i < unsupported; ++i)
        diagnosticsTrace("game.syscall.unsupported number=%lld calls=%llu", (long long)syscalls[i].number, (unsigned long long)syscalls[i].calls);
    LinuxDlStats dl = linuxDlStats();
    LinuxSdlStats sdl = linuxSdlStats();
    diagnosticsTrace("game.dl libraries=%u virtual=%u fallback_stub_bindings=%u tls_modules=%u", dl.libraries, dl.virtual_libraries, dl.unresolved_imports,
                     dl.tls_modules);
    diagnosticsTrace("game.sdl windows=%u contexts=%u frames=%u lookups=%u unknown_lookups=%u gl_ready=%d size=%ux%u", sdl.windows, sdl.contexts, sdl.frames,
                     sdl.calls, sdl.unknown_lookups, sdl.gl_ready, sdl.width, sdl.height);
    LinuxNetStats net = linuxNetStats();
    diagnosticsTrace("game.net sockets=%u open=%u connects=%u polls=%u resolves=%u failures=%u", net.sockets_created, net.sockets_open, net.connects, net.polls,
                     net.resolves, net.failures);
    diagnosticsTrace("game.trap.summary refused_calls=%llu requests=%u", (unsigned long long)trap_stats.refused, trap_stats.requests);
}

// ---- the client thread ------------------------------------------------------------------------------------------------------------
static void *clientEntry(void *argument) {
    (void)argument;
    // The program as a shell would start it, working directory /game. The client is a GraalVM native image: its garbage collector reads these
    // options from the command line. They give the young generation of the heap more room (fewer collections, so fewer small stalls) and the
    // heap a margin for it.
    static char program[] = "PokeMMO", heap[] = "-XX:MaxHeapSize=640m", young[] = "-XX:MaxNewSize=128m";
    static char *arguments[] = {program, heap, young, NULL};
    run.rc = ((MainFunction)run.main_entry)(3, arguments);
    atomic_store(&run.returned, true);
    return NULL;
}

bool gameThreadsLeft(void) { return abandoned; }

// Stops every thread the client created, for good: nothing of the game runs while the application leaves.
void gamePauseThreads(void) {
    LinuxThreadSnapshot threads[LINUX_THREAD_MAX_THREADS];
    unsigned count = linuxThreadsSnapshot(threads, LINUX_THREAD_MAX_THREADS);
    for (unsigned i = 0; i < count; ++i)
        if (threads[i].handle) svcSetThreadActivity(threads[i].handle, ThreadActivity_Paused);
}

const char *gameFailure(void) { return failure; }

static bool releaseAll(void) {
    bool ok = linuxThreadsReset();
    if (ok) {
        flushCapture();
        linuxStdioSetConsoleSink(NULL, NULL);
    }
    if (ok) ok = linuxStdioReset();
    if (ok) {
        linuxRuntimeSetExports(NULL, 0);
        linuxRuntimeReset();
        linuxTrapReset();
        crashRecorderSetRegions(NULL, 0);
    }
    if (ok) ok = linuxSdlReset();
    if (ok) ok = linuxDlReset();
    if (ok) ok = linuxTlsReset();
    if (ok) ok = linuxFilesReset();
    if (ok) ok = linuxSemReset();
    if (ok) ok = linuxSyncReset();
    if (ok) ok = linuxVmReleaseAll();
    if (ok && mapped.reservation) ok = elfExecutableClose(&mapped);
    return ok;
}

// The client's /tmp starts empty each run: a library half-written by an earlier failed run must not be mistaken for a good one.
static void wipeFolder(const char *folder, unsigned depth) {
    DIR *directory = opendir(folder);
    if (!directory) return;
    for (struct dirent *entry; (entry = readdir(directory)) != NULL;) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[512];
        if (snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) >= (int)sizeof(path)) continue;
        struct stat value;
        if (stat(path, &value)) continue;
        if (S_ISDIR(value.st_mode)) {
            if (depth < 8) wipeFolder(path, depth + 1);
            rmdir(path);
        } else
            unlink(path);
    }
    closedir(directory);
}

bool gameRun(const char *client_path) {
    failure = "";
    struct stat value;
    if ((mkdir(ROOT_DIR, 0700) && errno != EEXIST) || stat(ROOT_DIR, &value) || !S_ISDIR(value.st_mode)) {
        failure = "The isolate-root folder is not accessible on the SD card.";
        return false;
    }
    memset(&run, 0, sizeof(run));
    memset(pending_bytes, 0, sizeof(pending_bytes));
    linuxAudioOutAttach();
    linuxStdioSetConsoleSink(sink, NULL);
    linuxRuntimeReset();
    linuxTrapReset();
    bool ok = linuxFilesSetRoot(ROOT_DIR);
    // A running client expects /dev/urandom and friends and starts in its install directory.
    static const char *const folders[] = {ROOT_DIR "/tmp", ROOT_DIR "/home", ROOT_DIR "/home/switch"};
    for (unsigned i = 0; ok && i < sizeof(folders) / sizeof(folders[0]); ++i)
        if (mkdir(folders[i], 0700) && errno != EEXIST) ok = false;
    if (!ok) failure = "The isolate-root folder is not accessible on the SD card.";
    if (ok) {
        wipeFolder(ROOT_DIR "/tmp", 0);
        if (linuxAbiChdir("/game")) {
            failure = "The isolate-root/game folder is missing from the SD card.";
            ok = false;
        }
    }
    if (ok) ok = elfExecutableOpenResolved(client_path, &mapped, linuxTrapResolve, NULL);
    if (!ok || mapped.unresolved || mapped.unsupported) {
        if (!*failure) failure = "The PokeMMO client cannot be read (missing, corrupted or not enough memory).";
        releaseAll();
        return false;
    }
    // Shared libraries the client extracts and loads itself, and the ones this project implements (SDL3, EGL/GLX, OpenAL).
    static LinuxVirtualLibrary virtual_libraries[8];
    virtual_libraries[0] = linuxSdlLibrary;
    virtual_libraries[1] = linuxEglLibrary;
    virtual_libraries[2] = linuxOpenAlLibrary;
    virtual_libraries[3] = linuxGlxLibrary;
    for (unsigned i = 0; i < 4; ++i) virtual_libraries[4 + i] = linuxGtkLibraries[i];  // the file dialog's GTK (see linux_gtk.h)
    linuxDlSetVirtualLibraries(virtual_libraries, 8);
    linuxDlSetSearchDirectory("/lib");
    linuxDlSetFallbackResolver(linuxTrapResolve, NULL);
    linuxDlSetMainImage(&mapped);
    linuxRuntimeSetDynamicLoader(linuxDlLoaderHooks());
    run.main_entry = elfExecutableFindExport(&mapped, "main");
    unsigned count = 0;
    for (unsigned i = 0; i < mapped.export_count; ++i) {
        exports[count].name = mapped.exports[i].name;
        exports[count].address = mapped.exports[i].address;
        ++count;
    }
    linuxRuntimeSetExports(exports, count);
    crash_regions[0] = (CrashRegion){"client", (uintptr_t)mapped.base, mapped.layout.span};
    crashRecorderSetRegions(crash_regions, 1);
    diagnosticsTrace("game.mapping base=%p span=0x%llx slots_bound=%u trapped_imports=%u weak_null=%u exports=%u main=0x%llx", mapped.base,
                     (unsigned long long)mapped.layout.span, mapped.resolved, linuxTrapStats().count, mapped.weak_null, mapped.export_count,
                     (unsigned long long)run.main_entry);
    logState("before_start");
    if (!run.main_entry) {
        failure = "The PokeMMO client has no entry point.";
        releaseAll();
        return false;
    }

    LinuxPthreadAttr attr;
    linuxPthreadAttrInit(&attr);
    linuxPthreadAttrSetStackSize(&attr, STACK_BYTES);
    if (linuxPthreadCreate(&worker, &attr, clientEntry, NULL)) {
        failure = "The game's thread could not be created.";
        releaseAll();
        return false;
    }

    // The game ends by itself (its Exit button). The wait loop also listens to the system: when the application is asked to close (HOME menu),
    // the client sees a quit event. It takes no lock, so that nothing the game does can stop it.
    const uint64_t begin = milliseconds();
    bool finished = false;
    // Neither this loop nor the watchdog may run on the core of the game's main thread.
    int background = linuxThreadBackgroundCore();
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, background, 1u << background);
    startWatchdog();
    unsigned picture_width, picture_height, seen_width, seen_height;
    displayGameSize(&picture_width, &picture_height);
    seen_width = picture_width;
    seen_height = picture_height;
    uint64_t seen_since = milliseconds();
    uint64_t close_requested_at = 0;
    for (;;) {
        // Docking or undocking changes the size of the picture. The console flips between its two modes for a moment while it is docked, so a
        // new mode must last one second before the thread that renders is asked to apply it (between two frames).
        unsigned width, height;
        displayGameSize(&width, &height);
        if (width != seen_width || height != seen_height) {
            seen_width = width;
            seen_height = height;
            seen_since = milliseconds();
        }
        if ((seen_width != picture_width || seen_height != picture_height) && milliseconds() - seen_since >= 1000) {
            picture_width = seen_width;
            picture_height = seen_height;
            linuxSdlRequestSize(picture_width, picture_height);
        }
        if (!appletMainLoop() && !close_requested_at) {
            close_requested_at = milliseconds();
            linuxSdlStopRequest();
        }
        if (close_requested_at && milliseconds() - close_requested_at > CLOSE_GRACE_MS) break;
        svcSleepThread(50ull * 1000000ull);
        atomic_fetch_add(&wait_heartbeat, 1);
        // Done when main returned or when the client was unwound (exit/abort/trap) and its thread ended. Nothing here may wait for a lock.
        finished = atomic_load(&run.returned) || linuxThreadsLiveUnlocked() == 0 || linuxRuntimeTerminationKindUnlocked() != LINUX_TERMINATION_NONE;
        if (finished) break;
    }
    stopWatchdog();
    flushCapture();
    summary();
    if (!finished) {
        // The system asked the application to close and the game did not finish in time: it is left running and the application ends the process.
        abandoned = true;
        diagnosticsTrace("game.result=CLOSE_REQUESTED elapsed_ms=%u (the client thread is left running)", (unsigned)(milliseconds() - begin));
        logState("left");
        return true;
    }
    LinuxTermination termination = linuxRuntimeTermination();
    ok = atomic_load(&run.returned) && run.rc == 0 && termination.kind == LINUX_TERMINATION_NONE;
    logState("after_main");
    diagnosticsTrace("game.result=%s main_rc=%d termination_kind=%d status=%d elapsed_ms=%u", ok ? "MAIN_RETURNED" : "ENDED_WITH_ERROR", run.rc,
                     (int)termination.kind, termination.status, (unsigned)(milliseconds() - begin));
    if (!ok) failure = "The game stopped with an error.";
    if (linuxThreadsStats().live_threads) {
        // The Java runtime keeps its helper threads: they cannot be joined, so the run is left as it is.
        abandoned = true;
        diagnosticsTrace("game.threads_left live=%zu", linuxThreadsStats().live_threads);
        return ok;
    }
    return releaseAll() && ok;
}
