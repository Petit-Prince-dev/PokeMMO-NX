#pragma once
#include "linux_abi.h"

// Dynamic glibc CPU sets use 64-bit words on Linux AArch64.
#define LINUX_CPU_SET_MAX_BYTES (1024u * 1024u)
int linuxProcessGetpid(void);
void *linuxProcessCpuAlloc(size_t count);
void linuxProcessCpuFree(void *set);
int linuxProcessCpuCount(size_t bytes, const void *set);
int linuxProcessGetAffinity(int pid, size_t bytes, void *set);
int linuxProcessNanosleep(const LinuxTimespec *request, LinuxTimespec *remaining);
int64_t linuxProcessSysconf(int name);
