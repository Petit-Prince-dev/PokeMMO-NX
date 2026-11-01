#pragma once
#include <stdbool.h>

// Before the client owns the window or opens files. No display or writes when already current.
// False: the application was asked to close, or an interrupted installation cannot be recovered.
bool clientUpdateBeforeLaunch(bool network_ready);
const char *clientUpdateFailure(void);
