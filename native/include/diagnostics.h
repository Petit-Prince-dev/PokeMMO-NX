#pragma once
#include <stdarg.h>
#include <stdio.h>

// The diagnostics log: switch/PokeMMO/diagnostics.log, written only when the player created the file debug.enabled (see main.c).
// Everything that is traced goes through here, as whole lines.
void diagnosticsSetLog(FILE *log);                                                     // NULL: off
FILE *diagnosticsLog(void);                                                            // NULL when off; write only whole lines to it
void diagnosticsTrace(const char *format, ...) __attribute__((format(printf, 1, 2)));  // one line; the newline is added; flushed
void diagnosticsTraceV(const char *format, va_list args);
