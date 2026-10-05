#include "linux_audio.h"
#include "diagnostics.h"
#include "linux_threads.h"
#include <stdatomic.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

// The console's audio output (audout, 48 kHz stereo 16-bit) pulling blocks from the software mixer. A thread of its own keeps four
// blocks of 1024 frames (21 ms each) queued; when audout cannot be opened the thread still runs at the real-time pace and throws the
// sound away, so the game's streaming sources keep moving.
#define BLOCK_FRAMES 1024u
#define BLOCK_BYTES 0x1000u  // 1024 frames * 2 channels * 2 bytes, the size audout wants (a multiple of 0x1000)
#define BLOCKS 4u
#define THREAD_STACK 0x8000u
#define THREAD_PRIORITY 0x2c  // above the game's threads (0x3b): the mixer must not wait for them, and it blocks most of the time

static Thread thread;
static atomic_bool running;
static bool thread_started, audout_ready;
static AudioOutBuffer blocks[BLOCKS];
static int16_t *pcm[BLOCKS];
static atomic_uint late_blocks, refills, timeouts;

static void trace(const char *format, ...) {
    static atomic_uint lines;
    if (atomic_fetch_add(&lines, 1) > 40) return;
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}

static void queueBlock(unsigned index) {
    linuxAudioMix(pcm[index], BLOCK_FRAMES);
    blocks[index].next = NULL;
    blocks[index].buffer = pcm[index];
    blocks[index].buffer_size = BLOCK_BYTES;
    blocks[index].data_size = BLOCK_BYTES;
    blocks[index].data_offset = 0;
    audoutAppendAudioOutBuffer(&blocks[index]);
    atomic_fetch_add(&refills, 1);
}
static void silentPace(void) {
    static int16_t scratch[BLOCK_FRAMES * 2];
    u64 next = armGetSystemTick();
    const u64 period = armNsToTicks(21333333ull);
    while (atomic_load(&running)) {
        next += period;
        linuxAudioMix(scratch, BLOCK_FRAMES);
        u64 now = armGetSystemTick();
        if (now < next)
            svcSleepThread((s64)armTicksToNs(next - now));
        else
            next = now;
    }
}
static void outputThread(void *unused) {
    (void)unused;
    Result rc = audoutInitialize();
    bool initialized = R_SUCCEEDED(rc), started = false;
    if (initialized) {
        rc = audoutStartAudioOut();
        started = R_SUCCEEDED(rc);
    }
    audout_ready = started;
    trace("audio.out.init rc=0x%x rate=%u channels=%u format=%d", rc, started ? audoutGetSampleRate() : 0u, started ? audoutGetChannelCount() : 0u,
          started ? (int)audoutGetPcmFormat() : -1);
    for (unsigned i = 0; audout_ready && i < BLOCKS; ++i) {
        pcm[i] = (int16_t *)aligned_alloc(0x1000, BLOCK_BYTES);
        if (pcm[i])
            memset(pcm[i], 0, BLOCK_BYTES);
        else
            audout_ready = false;
    }
    if (audout_ready) {
        for (unsigned i = 0; i < BLOCKS; ++i) queueBlock(i);
        unsigned failures = 0;
        while (atomic_load(&running)) {
            AudioOutBuffer *released = NULL;
            u32 count = 0;
            u64 waited = armGetSystemTick();
            rc = audoutWaitPlayFinish(&released, &count, 100000000ull);
            if (R_FAILED(rc)) {
                atomic_fetch_add(&timeouts, 1);
                if (++failures > 50) {
                    trace("audio.out.wait_failed rc=0x%x: output left silent", rc);
                    break;
                }
            } else {
                failures = 0;
                if (armTicksToNs(armGetSystemTick() - waited) > 60000000ull) atomic_fetch_add(&late_blocks, 1);
            }
            // Several blocks can come back at once and libnx names only the first: every block audout no longer holds is refilled.
            for (unsigned i = 0; i < BLOCKS; ++i) {
                bool queued = true;
                if (R_SUCCEEDED(audoutContainsAudioOutBuffer(&blocks[i], &queued)) && !queued) queueBlock(i);
            }
        }
    }
    if (atomic_load(&running)) silentPace();  // no audio output (or it failed for good): keep the clock of the game's audio going
    if (started) audoutStopAudioOut();
    if (initialized) audoutExit();
    for (unsigned i = 0; i < BLOCKS; ++i) {
        free(pcm[i]);
        pcm[i] = NULL;
    }
    trace("audio.out.stopped refills=%u timeouts=%u late=%u", atomic_load(&refills), atomic_load(&timeouts), atomic_load(&late_blocks));
}

static bool outStart(void) {
    if (thread_started) return true;
    atomic_store(&running, true);
    atomic_store(&refills, 0);
    atomic_store(&timeouts, 0);
    atomic_store(&late_blocks, 0);
    Result rc = threadCreate(&thread, outputThread, NULL, NULL, THREAD_STACK, THREAD_PRIORITY, linuxThreadBackgroundCore());
    if (R_SUCCEEDED(rc)) rc = threadStart(&thread);
    trace("audio.out.thread rc=0x%x", rc);
    if (R_FAILED(rc)) {
        atomic_store(&running, false);
        return false;
    }
    thread_started = true;
    return true;
}
static void outStop(void) {
    if (!thread_started) return;
    atomic_store(&running, false);
    threadWaitForExit(&thread);
    threadClose(&thread);
    thread_started = false;
}
// A holder with a lower priority than the mixer must get time to leave the mixer's lock: sleep instead of spinning.
static void outYield(void) { svcSleepThread(50000); }

void linuxAudioOutAttach(void) {
    static const LinuxAudioOutput hooks = {outStart, outStop, outYield};
    linuxAudioSetOutput(&hooks);
}
