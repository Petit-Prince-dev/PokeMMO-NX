#pragma once
#include "linux_abi.h"
#include <stdarg.h>
#define LINUX_FORMAT_MAX_OUTPUT (1024u * 1024u)
// AAPCS64 va_list; Linux LP64 integer formats.
int linuxFormatSnprintf(char *, size_t, const char *, ...);
int linuxFormatVsnprintf(char *, size_t, const char *, va_list);
int linuxFormatFprintf(void *, const char *, ...);
int linuxFormatVfprintf(void *, const char *, va_list);
