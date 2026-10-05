#pragma once
#include "linux_abi.h"
#include <stdio.h>
#define LINUX_STDIO_MAX 32u
#define LINUX_STDIO_BUFFER 4096u
// Opaque managed streams. These tokens are NOT glibc/newlib FILE structures.
// Only adapter functions may consume them.
extern void *linuxStdioStdout, *linuxStdioStderr;  // addresses of pointer objects, opaque targets
typedef int64_t (*LinuxConsoleSink)(void *, int, const void *, size_t, int *);
void linuxStdioSetConsoleSink(LinuxConsoleSink, void *);                     // set while no thread writes
int64_t linuxStdioWriteConsoleFd(int fd, const void *buffer, size_t count);  // raw write(1|2): same sink as buffered output
int64_t linuxStdioWriteRecord(void *, const void *, size_t);                 // one lock; validates even zero bytes
void *linuxStdioFopen(const char *, const char *);
void *linuxStdioFdopen(int, const char *);
int linuxStdioFclose(void *);
size_t linuxStdioFread(void *, size_t, size_t, void *);
size_t linuxStdioFwrite(const void *, size_t, size_t, void *);
int linuxStdioFflush(void *);
int linuxStdioFeof(void *);
int linuxStdioFerror(void *);
void linuxStdioClearerr(void *);
char *linuxStdioFgets(char *, int, void *);
int linuxStdioFputs(const char *, void *);
int linuxStdioFileno(void *);
int linuxStdioFseeko(void *, int64_t, int);
int64_t linuxStdioFtello(void *);
bool linuxStdioHasStreams(void);
bool linuxStdioReset(void);  // after joining workers, BEFORE file reset
