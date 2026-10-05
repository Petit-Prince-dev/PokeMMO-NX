#pragma once
#include "linux_dl.h"

// The small part of GTK 3 that the game's file dialog library (liblwjgl_nfd) imports, as virtual libraries: libgtk-3, libgdk-3, libglib-2.0 and
// libgobject-2.0 all answer from the same table. The dialog it builds is shown by the chooser of linux_file_picker.c.
extern const LinuxVirtualLibrary linuxGtkLibraries[4];
