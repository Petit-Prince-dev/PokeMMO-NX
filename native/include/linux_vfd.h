#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Descriptors that are not files nor sockets: eventfd, pipes and epoll instances, kept in the memory of the process, plus the one
// place where the guest waits for descriptors (poll and epoll_wait). They share the descriptor table of linux_files.c.
//
// Java's selectors need all of them: a selector is an epoll instance with an eventfd (or pipe) that another thread writes to in
// order to wake it up. epoll_wait cannot sleep inside the BSD service for an in-memory descriptor, so while one is involved the
// wait is done in short slices (a few milliseconds), which is also the worst delay of a wake-up.
enum { LINUX_VFD_EVENTFD = 5, LINUX_VFD_PIPE = 6, LINUX_VFD_EPOLL = 7 };

typedef struct {
    uint32_t events, padding;
    uint64_t data;
} LinuxEpollEvent;  // glibc AArch64: not packed, 16 bytes
_Static_assert(sizeof(LinuxEpollEvent) == 16, "AArch64 struct epoll_event");

// One entry of a wait: wanted and ready are LINUX_POLL* bits (linux_net_translate.h).
typedef struct {
    int fd;
    unsigned wanted, ready;
} LinuxVfdWait;
// Fills `ready` of every entry and waits up to timeout_ms (negative: for ever) for at least one to become ready. Returns how many are
// ready, or -1 with *error (a Linux errno).
int linuxVfdWait(LinuxVfdWait *entries, unsigned count, int timeout_ms, int *error);

// Object management, called by the descriptor table (linux_files.c).
void linuxVfdRelease(int kind, int object, int access);
int64_t linuxVfdRead(int kind, int object, void *buffer, size_t count, bool nonblock, int *error);
int64_t linuxVfdWrite(int kind, int object, const void *buffer, size_t count, bool nonblock, int *error);

// Guest ABI.
int linuxAbiEventfd(unsigned initial, int flags);
int linuxAbiPipe(int descriptors[2]);
int linuxAbiPipe2(int descriptors[2], int flags);
int linuxAbiEpollCreate(int size);
int linuxAbiEpollCreate1(int flags);
int linuxAbiEpollCtl(int epoll, int operation, int fd, const LinuxEpollEvent *event);
int linuxAbiEpollWait(int epoll, LinuxEpollEvent *events, int max, int timeout_ms);
int linuxAbiEpollPwait(int epoll, LinuxEpollEvent *events, int max, int timeout_ms, const void *sigmask);
