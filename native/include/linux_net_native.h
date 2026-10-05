#pragma once
#include "linux_net_translate.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// The socket adapters' native side: Horizon's BSD service through libnx. Handles are the native descriptors.
// Structures and constants are Horizon's BSD ones (linux_net_translate.h); every error comes back as a LINUX errno in *error
// and the return value is then -1. getaddrinfo failures come back as glibc EAI_* numbers (negative).
int linuxNetNativeSocket(int type, int protocol, int *error);
int linuxNetNativeClose(int handle);
int linuxNetNativeSetNonblocking(int handle, bool enable, int *error);
int linuxNetNativeConnect(int handle, const BsdSockaddrIn *address, int *error);
int linuxNetNativeBind(int handle, const BsdSockaddrIn *address, int *error);
int linuxNetNativeListen(int handle, int backlog, int *error);
int linuxNetNativeAccept(int handle, BsdSockaddrIn *address, int *error);  // returns the new handle
int linuxNetNativeName(int handle, bool peer, BsdSockaddrIn *address, int *error);
int linuxNetNativeSetsockopt(int handle, int level, int name, const void *value, unsigned length, int *error);
int linuxNetNativeGetsockopt(int handle, int level, int name, void *value, unsigned *length, int *error);
int linuxNetNativeShutdown(int handle, int how, int *error);
int64_t linuxNetNativeSend(int handle, const void *buffer, size_t count, int flags, const BsdSockaddrIn *to, int *error);
int64_t linuxNetNativeRecv(int handle, void *buffer, size_t count, int flags, BsdSockaddrIn *from, int *error);
typedef struct {
    int handle;
    int16_t events, revents;
} LinuxNetPollEntry;                                                                             // BSD event bits
int linuxNetNativePoll(LinuxNetPollEntry *entries, unsigned count, int timeout_ms, int *error);  // count 0: just wait; returns the number ready

#define LINUX_NET_MAX_RESOLVED 8u
typedef struct {
    unsigned count;
    struct {
        uint32_t address;
        uint16_t port;
        int32_t socktype, protocol;
    } entries[LINUX_NET_MAX_RESOLVED];  // network order
    char canonical_name[256];
} LinuxNetResolved;
// flags are glibc AI_* bits; returns 0 or a negative glibc EAI_* number. Only IPv4 results are returned.
int linuxNetNativeResolve(const char *node, const char *service, int flags, int socktype, int protocol, LinuxNetResolved *result);
// The console's own IPv4 address and netmask, in network order; false while it has no network.
bool linuxNetNativeLocalNetwork(uint32_t *address, uint32_t *netmask);
