#include "diagnostics.h"
#include <stdatomic.h>

static _Atomic(FILE *) log_file;

void diagnosticsSetLog(FILE *log) { atomic_store_explicit(&log_file, log, memory_order_release); }
FILE *diagnosticsLog(void) { return atomic_load_explicit(&log_file, memory_order_acquire); }

void diagnosticsTraceV(const char *format, va_list args) {
    FILE *log = diagnosticsLog();
    if (!log) return;
    flockfile(log);
    vfprintf(log, format, args);
    fputc('\n', log);
    fflush(log);
    funlockfile(log);
}

void diagnosticsTrace(const char *format, ...) {
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}
