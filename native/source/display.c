#include "display.h"
#include "diagnostics.h"
#include <switch.h>

// GNU --wrap (see the Makefile) redirects the linked libnx/Mesa calls through the functions below.
Result __real_framebufferCreate(Framebuffer *fb, NWindow *window, u32 width, u32 height, u32 format, u32 count);
Result __real_nwindowDequeueBuffer(NWindow *window, s32 *slot, NvMultiFence *fence);

bool displayPrepareWindow(void) {
    NWindow *window = nwindowGetDefault();
    bool ready = nwindowIsValid(window) && !window->slots_configured && window->cur_slot < 0;
    if (!ready) {
        diagnosticsTrace("display.window=BUSY connected=%u slots=0x%llx current=%d", window->is_connected, (unsigned long long)window->slots_configured,
                         window->cur_slot);
        return false;
    }
    // libnx 4.12.0 resets the format to 0 on disconnect, but ConfigureBuffer only adopts the next buffer's format when it is ~0U (the
    // creation sentinel). Both the software console and Mesa release/disconnect this shared window: restore automatic format selection
    // while no buffers are registered (nx/source/display/native_window.c: _nwindowDisconnect and nwindowConfigureBuffer, libnx v4.12.0).
    window->format = ~0U;
    return true;
}

void displayGameSize(unsigned *width, unsigned *height) {
    bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    *width = docked ? 1920 : 1280;
    *height = docked ? 1080 : 720;
}

void displaySetGameResolution(void) {
    unsigned width, height;
    displayGameSize(&width, &height);
    Result rc = nwindowSetDimensions(nwindowGetDefault(), width, height);
    diagnosticsTrace("display.resolution %ux%u rc=0x%x", width, height, rc);
}

Result __wrap_framebufferCreate(Framebuffer *fb, NWindow *window, u32 width, u32 height, u32 format, u32 count) {
    // Mesa registers slots 0, 1 and 2. Keep the default software console's allocation large enough for every slot when it reuses that
    // same producer: libnx's console asks for two RGB565 buffers and framebufferEnd uses cur_slot * fb_size without checking num_fbs.
    if (window == nwindowGetDefault() && format == PIXEL_FORMAT_RGB_565 && count == 2) count = 3;
    return __real_framebufferCreate(fb, window, width, height, format, count);
}

Result __wrap_nwindowDequeueBuffer(NWindow *window, s32 *out_slot, NvMultiFence *fence) {
    s32 slot = -1;
    Result rc = __real_nwindowDequeueBuffer(window, &slot, fence);
    if (R_SUCCEEDED(rc) && (slot < 0 || slot >= 64 || !(window->slots_configured & (1ULL << slot)))) {
        diagnosticsTrace("display.dequeue=INVALID_SLOT slot=%d configured=0x%llx", slot, (unsigned long long)window->slots_configured);
        // Never let framebufferEnd or Mesa index storage for an unregistered slot: keep the original error path.
        if (slot >= 0 && slot < 64) nwindowCancelBuffer(window, slot, fence);
        rc = MAKERESULT(Module_Libnx, LibnxError_BadGfxDequeueBuffer);
    }
    if (R_SUCCEEDED(rc) && out_slot) *out_slot = slot;
    return rc;
}
