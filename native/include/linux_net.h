#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Sockets for the guest (Linux AArch64, glibc ABI) on top of Horizon's BSD service.
//
// Socket descriptors share the descriptor table of the file adapters (linux_files.c), so read/write/close/fcntl/fstat on a
// socket work like on a file. Only IPv4 TCP and UDP exist: AF_INET6 is refused with EAFNOSUPPORT, which makes Java fall back to
// IPv4. The native side (linux_net_native.h) speaks Horizon's BSD structures and constants; this file translates the
// Linux layouts, constants and error numbers (linux_net_translate.h) and keeps the descriptor bookkeeping.

// Guest layouts.
typedef struct {
    int32_t fd;
    int16_t events, revents;
} LinuxPollfd;
typedef struct {
    int32_t flags, family, socktype, protocol;
    uint32_t address_length, padding;
    void *address;
    char *canonical_name;
    void *next;
} LinuxAddrinfo;
_Static_assert(sizeof(LinuxPollfd) == 8, "glibc struct pollfd");
_Static_assert(sizeof(LinuxAddrinfo) == 48, "glibc AArch64 struct addrinfo");

int linuxAbiSocket(int domain, int type, int protocol);
int linuxAbiConnect(int fd, const void *address, unsigned length);
int linuxAbiBind(int fd, const void *address, unsigned length);
int linuxAbiListen(int fd, int backlog);
int linuxAbiAccept(int fd, void *address, unsigned *length);
int linuxAbiAccept4(int fd, void *address, unsigned *length, int flags);
int linuxAbiGetsockname(int fd, void *address, unsigned *length);
int linuxAbiGetpeername(int fd, void *address, unsigned *length);
int linuxAbiSetsockopt(int fd, int level, int name, const void *value, unsigned length);
int linuxAbiGetsockopt(int fd, int level, int name, void *value, unsigned *length);
int linuxAbiShutdown(int fd, int how);
int64_t linuxAbiSend(int fd, const void *buffer, size_t count, int flags);
int64_t linuxAbiRecv(int fd, void *buffer, size_t count, int flags);
int64_t linuxAbiSendto(int fd, const void *buffer, size_t count, int flags, const void *address, unsigned length);
int64_t linuxAbiRecvfrom(int fd, void *buffer, size_t count, int flags, void *address, unsigned *length);
int linuxAbiPoll(LinuxPollfd *fds, unsigned long count, int timeout_ms);
int linuxAbiIoctl(int fd, unsigned long request, ...);
int linuxAbiGetaddrinfo(const char *node, const char *service, const LinuxAddrinfo *hints, LinuxAddrinfo **result);
void linuxAbiFreeaddrinfo(LinuxAddrinfo *list);
const char *linuxAbiGaiStrerror(int code);
int linuxAbiInetPton(int family, const char *text, void *destination);
int linuxAbiGethostname(char *name, size_t length);

typedef struct {
    unsigned sockets_created, sockets_open, connects, polls, resolves, failures;
} LinuxNetStats;
LinuxNetStats linuxNetStats(void);
