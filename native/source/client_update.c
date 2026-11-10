#include "client_update.h"
#include "client_update_internal.h"
#include "diagnostics.h"
#include <stdlib.h>
#include <sys/statvfs.h>
#include <switch.h>

static UpdateJob job;
static const char *failure;
uint64_t updateNow(void) { return armTicksToNs(armGetSystemTick()) / 1000000ull; }
bool updateCancelled(const UpdateJob *state) {
    return atomic_load(&state->cancel) || (atomic_load(&state->phase) == UPDATE_CHECK && updateNow() >= atomic_load(&state->check_deadline));
}
void updateProgress(UpdateJob *state, UpdatePhase phase, unsigned progress) {
    atomic_store(&state->progress, progress);
    atomic_store(&state->phase, phase);
}
static void worker(void *argument) {
    UpdateJob *state = argument;
    state->launch = updateRecover(state);
    if (!state->launch) {
        updateProgress(state, UPDATE_BROKEN, 0);
        atomic_store(&state->finished, true);
        return;
    }
    atomic_store(&state->check_deadline, updateNow() + 4000);
    updateProgress(state, UPDATE_CHECK, 0);
    uint32_t installed = updateInstalledRevision(UPDATE_GAME "/revision.txt");
    UpdateHttp *http = state->network_ready && installed && !updateCancelled(state) ? updateHttpOpen(state) : NULL;
    UpdateRemote remote = {0};
    bool checked = http && updateRemoteRevision(http, &remote) && !updateCancelled(state);
    if (!checked) {
        updateProgress(state, UPDATE_CHECK_FAILED, 0);
    } else if (remote.revision > installed) {
        diagnosticsTrace("update.detected installed=%u remote=%u", installed, remote.revision);
        updateProgress(state, UPDATE_DOWNLOAD, 0);
        UpdateEntry *entries = NULL;
        unsigned count = 0;
        struct statvfs storage;
        uint64_t needed = remote.bytes + remote.expanded + 16 * 1024 * 1024;
        bool space = !statvfs(UPDATE_ROOT, &storage) && storage.f_bavail && storage.f_frsize &&
                     needed / storage.f_frsize + (needed % storage.f_frsize != 0) <= storage.f_bavail;
        bool ok = space && updateClean() && updateMakeParents(UPDATE_ARCHIVE) && updateHttpDownload(http, &remote) &&
                  updatePrepare(state, remote.revision, &entries, &count);
        if (ok) ok = updateInstall(state, entries, count);
        free(entries);
        if (!ok) {
            state->launch = updateRecover(state);
            updateProgress(state, state->launch ? UPDATE_FAILED : UPDATE_BROKEN, 0);
        } else {
            updateProgress(state, UPDATE_DONE, 1000);
            diagnosticsTrace("update.installed revision=%u", remote.revision);
        }
    }
    updateHttpClose(http);
    atomic_store(&state->finished, true);
}
static bool showCheckFailure(void) {
    diagnosticsTrace("update.check=FAILED");
    bool running = appletMainLoop();
    if (running && updateScreenOpen()) {
        updateScreenDraw(0, "Couldn't check for game updates.", "Starting the game. Check your internet connection.");
        uint64_t deadline = updateNow() + 3000;
        while (running && updateNow() < deadline) {
            svcSleepThread(16000000);
            running = appletMainLoop();
        }
    }
    updateScreenClose();
    return running;
}
const char *clientUpdateFailure(void) { return failure ? failure : ""; }
bool clientUpdateBeforeLaunch(bool network_ready) {
    failure = NULL;
    atomic_store(&job.cancel, false);
    atomic_store(&job.finished, false);
    atomic_store(&job.phase, UPDATE_CHECK);
    atomic_store(&job.progress, 0);
    atomic_store(&job.resolver, 0);
    job.network_ready = network_ready;
    atomic_store(&job.check_deadline, updateNow() + 4000);
    Thread thread;
    Result rc = threadCreate(&thread, worker, &job, NULL, 256 * 1024, 0x3b, -2);
    if (R_FAILED(rc)) return showCheckFailure();
    rc = threadStart(&thread);
    if (R_FAILED(rc)) {
        threadClose(&thread);
        return showCheckFailure();
    }
    bool quit = false, screen_ready = false;
    uint64_t drawn_at = 0;
    while (!atomic_load(&job.finished)) {
        if (!appletMainLoop()) quit = true;
        if (quit) atomic_store(&job.cancel, true);
        if (quit || (atomic_load(&job.phase) == UPDATE_CHECK && updateNow() >= atomic_load(&job.check_deadline))) {
            u32 handle = atomic_exchange(&job.resolver, 0);
            if (handle) resolverCancel(handle);
        }
        unsigned phase = atomic_load(&job.phase);
        bool updating = phase == UPDATE_DOWNLOAD || phase == UPDATE_EXTRACT || phase == UPDATE_INSTALL;
        if (!quit && updating && updateNow() - drawn_at >= 100) {
            if (!screen_ready) screen_ready = updateScreenOpen();
            updateScreenDraw(atomic_load(&job.progress), "PokeMMO client update detected, updating...", NULL);
            drawn_at = updateNow();
        }
        svcSleepThread(10000000);
    }
    threadWaitForExit(&thread);
    threadClose(&thread);
    unsigned phase = atomic_load(&job.phase);
    if (!quit && phase == UPDATE_CHECK_FAILED) return showCheckFailure();
    if (!quit && (phase == UPDATE_FAILED || phase == UPDATE_BROKEN)) {
        if (!screen_ready) screen_ready = updateScreenOpen();
        if (screen_ready) {
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            PadState pad;
            padInitializeDefault(&pad);
            updateScreenDraw(0, "Unable to update the PokeMMO client.",
                             job.launch ? "Press A to start the installed client." : "Press A to quit and retry next time.");
            do {
                if (!appletMainLoop()) {
                    quit = true;
                    break;
                }
                padUpdate(&pad);
                if (padGetButtonsDown(&pad) & HidNpadButton_A) break;
                svcSleepThread(16000000);
            } while (true);
        }
        if (!job.launch && !screen_ready)
            failure = "The interrupted client update could not be restored. Reinstall the client files, keeping your config and ROMs.";
    }
    updateScreenClose();
    return !quit && job.launch;
}
