#pragma once
#include "linux_sdl_events.h"
#include <stdbool.h>
#include <stddef.h>

// The game's "choose a file or folder" dialog (GTK on Linux, which a Switch does not have). A small chooser is drawn over the game with
// OpenGL and driven by the controller and the touch screen: A opens a folder or picks a file, B goes up, X cancels, Y chooses the current
// folder, minus changes the file filter, L/R scroll by pages; a tap picks an entry and a drag scrolls. The game sees the folders it
// can see (isolate-root as "/") and the whole SD card as "/sd".
enum { LINUX_PICKER_OPEN = 0, LINUX_PICKER_SAVE = 1, LINUX_PICKER_FOLDER = 2, LINUX_PICKER_CREATE_FOLDER = 3 };  // GtkFileChooserAction
#define LINUX_PICKER_FILTERS 8u
#define LINUX_PICKER_PATTERNS 8u
typedef struct {
    int action;
    const char *title, *folder, *name;  // folder: where to start (a game path), NULL for the default; name: suggested file name when saving
    unsigned filter_count;              // file filters: a name and glob patterns such as "*.nds"
    const char *filter_names[LINUX_PICKER_FILTERS];
    unsigned pattern_counts[LINUX_PICKER_FILTERS];
    const char *patterns[LINUX_PICKER_FILTERS][LINUX_PICKER_PATTERNS];
} LinuxPickerRequest;

// Shows the chooser and waits for the player. True: `result` holds the chosen game path. One chooser at a time; may be called from any
// thread, including the one that renders the game.
bool linuxFilePickerRun(const LinuxPickerRequest *request, char *result, size_t result_size);

// Used by the SDL layer.
bool linuxFilePickerOpen(void);                                // the chooser is on the screen
bool linuxFilePickerCapturesInput(void);                       // the chooser is open, or its closing buttons have not been released yet
void linuxFilePickerFeed(const LinuxInputSnapshot *snapshot);  // the sampled controller and touch screen, while the chooser captures them
void linuxFilePickerDraw(unsigned width, unsigned height);     // on the rendering thread, before the buffers are swapped
void linuxFilePickerReset(void);
