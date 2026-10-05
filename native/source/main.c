#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <switch.h>
#include "diagnostics.h"
#include "display.h"
#include "game.h"
#include "linux_sdl.h"
#include "linux_vm.h"

// libnx's exit sequence (not declared in its headers): closes the services, then returns to whatever the exit function points at.
extern void __libnx_exit(int result) __attribute__((noreturn));
extern u32 __nx_applet_exit_mode;

#define ROOT "sdmc:/switch/PokeMMO"
// The client install folder as the game expects it (bin/linux/arm64/PokeMMO, data/, config/ ...), inside the files root.
#define CLIENT_PATH ROOT "/isolate-root/game/bin/linux/arm64/PokeMMO"
// Diagnostics are off unless this file exists; they are then written to the log below.
#define DEBUG_FLAG ROOT "/debug.enabled"
#define DEBUG_LOG ROOT "/diagnostics.log"

// A message on the console screen, until + is pressed. Only used when the game cannot start or ended with an error.
static void showMessage(const char *text) {
    displayPrepareWindow();
    PrintConsole *console = consoleInit(NULL);
    if (!console || !console->consoleInitialised) return;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    consoleClear();
    printf("\n\n   PokeMMO\n\n   %s\n\n   Press + to quit.\n\n", text);
    printf("   For a diagnostics log, create an empty file named debug.enabled in\n   switch/PokeMMO on the SD card, restart, then read diagnostics.log.\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    // Mesa (the game's OpenGL) skips its checks of the arguments of every GL call: a little less processor time per frame, but a call that is
    // wrong would no longer be refused. Read when the GL context is created.
    setenv("MESA_NO_ERROR", "1", 1);
    mkdir("sdmc:/switch", 0777);
    mkdir(ROOT, 0777);
    struct stat info;
    FILE *log = NULL;
    if (stat(DEBUG_FLAG, &info) == 0 && (log = fopen(DEBUG_LOG, "w"))) {
        diagnosticsSetLog(log);
        diagnosticsTrace("app.version=" APP_VERSION " hos=%u.%u.%u applet=%u", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()),
                         HOSVER_MICRO(hosversionGet()), appletGetAppletType());
    }
    bool sockets_ready = R_SUCCEEDED(socketInitializeDefault());
    bool nifm_ready = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    diagnosticsTrace("services sockets=%d nifm=%d", sockets_ready, nifm_ready);

    if (appletGetAppletType() != AppletType_Application)
        showMessage("Applet mode: not enough memory. Restart hbmenu while holding R when launching a game (application mode).");
    else if (stat(CLIENT_PATH, &info) != 0)
        showMessage("The PokeMMO client was not found in switch/PokeMMO/isolate-root/game.");
    else {
        // The game draws through EGL on the console's window. The text console is created and released once before it takes over: that is
        // the sequence validated on the Switch, and it leaves the window in the state Mesa expects (only a blank frame is drawn).
        PrintConsole *console = consoleInit(NULL);
        if (console) {
            consoleUpdate(NULL);
            consoleExit(NULL);
        }
        displayPrepareWindow();
        displaySetGameResolution();
        bool left = gameRun(CLIENT_PATH);
        // The client may leave its EGL surface bound to the console window: release it so that a message can be shown if needed.
        linuxSdlReset();
        if (!left) showMessage(gameFailure());
    }

    diagnosticsTrace("app.exit threads_left=%d", gameThreadsLeft());
    if (nifm_ready) nifmExit();
    if (sockets_ready) socketExit();
    if (gameThreadsLeft()) {
        // The client's own threads are still running and cannot be ended: returning would hand the process back to hbloader, which loads the
        // menu again in the same process on memory they use. Flush what the game wrote through memory-mapped files, stop the threads, then
        // leave the way a game does: libnx closes the services, tells the system that the application is exiting (a bare process exit would
        // show the "an error has occurred" screen) and the system ends the process.
        linuxVmFlushFileMappings();
        gamePauseThreads();
        diagnosticsSetLog(NULL);  // every line was flushed as it was written: the log is not closed, a stopped thread may hold its lock
        __nx_applet_exit_mode = 1;
        __libnx_exit(0);
    }
    diagnosticsSetLog(NULL);
    if (log) fclose(log);
    return 0;
}
