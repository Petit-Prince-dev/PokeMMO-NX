#include "linux_vfd.h"
#include "linux_abi.h"
#include "linux_files.h"
#include "linux_net_native.h"
#include "linux_net_translate.h"
#include "linux_sync.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define MAX_OBJECTS 64u
#define PIPE_BYTES (64u * 1024u)
#define MAX_INTEREST 64u
#define WAIT_SLICE_MS 3
enum { O_NONBLOCK_FLAG = 0x800, O_CLOEXEC_FLAG = 0x80000, EFD_SEMAPHORE = 1, EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3 };
enum { EPOLLIN = 1, EPOLLOUT = 4, EPOLLERR = 8, EPOLLHUP = 0x10 };

typedef struct {
    bool used;
    uint64_t counter;
    bool semaphore;
} EventFd;
typedef struct {
    bool used, read_open, write_open;
    unsigned char *data;
    size_t head, count;
} Pipe;
typedef struct {
    int fd;
    uint32_t events;
    uint64_t data;
} Interest;
typedef struct {
    bool used;
    Interest interest[MAX_INTEREST];
    unsigned count;
} Epoll;
static EventFd eventfds[MAX_OBJECTS];
static Pipe pipes[MAX_OBJECTS];
static Epoll epolls[MAX_OBJECTS];

static int fail(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}

// ---- eventfd ----------------------------------------------------------------------------------------------------------------------
static int createEventfd(unsigned long long initial, bool semaphore) {
    linuxSyncLock();
    int index = -1;
    for (unsigned i = 0; i < MAX_OBJECTS && index < 0; ++i)
        if (!eventfds[i].used) index = (int)i;
    if (index >= 0) eventfds[index] = (EventFd){.used = true, .counter = initial, .semaphore = semaphore};
    linuxSyncUnlock();
    return index;
}
static void sleepMs(unsigned milliseconds) { svcSleepThread((int64_t)milliseconds * 1000000ll); }
static uint64_t nowMs(void) { return armTicksToNs(armGetSystemTick()) / 1000000ull; }
static void sleepSlice(void) { sleepMs(1); }

int64_t linuxVfdRead(int kind, int object, void *buffer, size_t count, bool nonblock, int *error) {
    if (object < 0 || (unsigned)object >= MAX_OBJECTS) {
        *error = LINUX_EBADF;
        return -1;
    }
    for (;;) {
        linuxSyncLock();
        if (kind == LINUX_VFD_EVENTFD && eventfds[object].used) {
            EventFd *event = &eventfds[object];
            if (count < 8) {
                linuxSyncUnlock();
                *error = LINUX_EINVAL;
                return -1;
            }
            if (event->counter) {
                uint64_t value = event->semaphore ? 1 : event->counter;
                event->counter -= value;
                memcpy(buffer, &value, 8);
                linuxSyncUnlock();
                return 8;
            }
        } else if (kind == LINUX_VFD_PIPE && pipes[object].used) {
            Pipe *pipe = &pipes[object];
            if (pipe->count) {
                size_t take = count < pipe->count ? count : pipe->count;
                for (size_t i = 0; i < take; ++i) ((unsigned char *)buffer)[i] = pipe->data[(pipe->head + i) % PIPE_BYTES];
                pipe->head = (pipe->head + take) % PIPE_BYTES;
                pipe->count -= take;
                linuxSyncUnlock();
                return (int64_t)take;
            }
            if (!pipe->write_open || !count) {
                linuxSyncUnlock();
                return 0;
            }  // end of file
        } else {
            linuxSyncUnlock();
            *error = LINUX_EBADF;
            return -1;
        }
        linuxSyncUnlock();
        if (nonblock) {
            *error = LINUX_EAGAIN;
            return -1;
        }
        sleepSlice();
    }
}

int64_t linuxVfdWrite(int kind, int object, const void *buffer, size_t count, bool nonblock, int *error) {
    if (object < 0 || (unsigned)object >= MAX_OBJECTS) {
        *error = LINUX_EBADF;
        return -1;
    }
    size_t done = 0;
    for (;;) {
        linuxSyncLock();
        if (kind == LINUX_VFD_EVENTFD && eventfds[object].used) {
            EventFd *event = &eventfds[object];
            uint64_t value;
            if (count < 8) {
                linuxSyncUnlock();
                *error = LINUX_EINVAL;
                return -1;
            }
            memcpy(&value, buffer, 8);
            if (value == UINT64_MAX) {
                linuxSyncUnlock();
                *error = LINUX_EINVAL;
                return -1;
            }
            if (event->counter <= UINT64_MAX - 1 - value) {
                event->counter += value;
                linuxSyncUnlock();
                return 8;
            }
        } else if (kind == LINUX_VFD_PIPE && pipes[object].used) {
            Pipe *pipe = &pipes[object];
            if (!pipe->read_open) {
                linuxSyncUnlock();
                *error = LINUX_EPIPE;
                return -1;
            }
            size_t room = PIPE_BYTES - pipe->count, take = count - done < room ? count - done : room;
            for (size_t i = 0; i < take; ++i) pipe->data[(pipe->head + pipe->count + i) % PIPE_BYTES] = ((const unsigned char *)buffer)[done + i];
            pipe->count += take;
            done += take;
            linuxSyncUnlock();
            if (done == count) return (int64_t)done;
            if (nonblock) {
                if (done) return (int64_t)done;
                *error = LINUX_EAGAIN;
                return -1;
            }  // a blocking write fills the pipe completely
            sleepSlice();
            continue;
        } else {
            linuxSyncUnlock();
            *error = LINUX_EBADF;
            return -1;
        }
        linuxSyncUnlock();
        if (nonblock) {
            *error = LINUX_EAGAIN;
            return -1;
        }
        sleepSlice();
    }
}

void linuxVfdRelease(int kind, int object, int access) {
    if (object < 0 || (unsigned)object >= MAX_OBJECTS) return;
    linuxSyncLock();
    if (kind == LINUX_VFD_EVENTFD)
        memset(&eventfds[object], 0, sizeof(eventfds[object]));
    else if (kind == LINUX_VFD_EPOLL)
        memset(&epolls[object], 0, sizeof(epolls[object]));
    else if (kind == LINUX_VFD_PIPE && pipes[object].used) {
        Pipe *pipe = &pipes[object];
        if (access == 0)
            pipe->read_open = false;
        else
            pipe->write_open = false;
        if (!pipe->read_open && !pipe->write_open) {
            free(pipe->data);
            memset(pipe, 0, sizeof(*pipe));
        }
    }
    linuxSyncUnlock();
}

// ---- waiting ----------------------------------------------------------------------------------------------------------------------
// Which of the wanted events an in-memory descriptor has now (kind and object index as the descriptor table keeps them).
static unsigned linuxVfdReady(int kind, int object, int access, unsigned wanted) {
    if (object < 0 || (unsigned)object >= MAX_OBJECTS) return LINUX_POLLNVAL;
    unsigned ready = 0;
    linuxSyncLock();
    if (kind == LINUX_VFD_EVENTFD && eventfds[object].used) {
        if (eventfds[object].counter) ready |= LINUX_POLLIN;
        if (eventfds[object].counter < UINT64_MAX - 1) ready |= LINUX_POLLOUT;
    } else if (kind == LINUX_VFD_PIPE && pipes[object].used) {
        const Pipe *pipe = &pipes[object];
        if (access == 0) {
            if (pipe->count) ready |= LINUX_POLLIN;
            if (!pipe->write_open) ready |= LINUX_POLLHUP;
        } else {
            if (pipe->count < PIPE_BYTES) ready |= LINUX_POLLOUT;
            if (!pipe->read_open) ready |= LINUX_POLLERR;
        }
    } else if (kind == LINUX_VFD_EPOLL && epolls[object].used) {
        // an epoll descriptor inside another is not supported: never ready
    } else
        ready = LINUX_POLLNVAL;
    linuxSyncUnlock();
    return ready & (wanted | LINUX_POLLERR | LINUX_POLLHUP | LINUX_POLLNVAL);
}
int linuxVfdWait(LinuxVfdWait *entries, unsigned count, int timeout_ms, int *error) {
    uint64_t start = nowMs();
    for (;;) {
        unsigned ready_count = 0, sockets = 0;
        LinuxNetPollEntry native[64];
        unsigned index_of[64];
        bool in_memory_waiter = false;
        if (count > 64) {
            *error = LINUX_EINVAL;
            return -1;
        }
        for (unsigned i = 0; i < count; ++i) {
            LinuxVfdWait *entry = &entries[i];
            entry->ready = 0;
            if (entry->fd < 0) continue;
            int handle, kind, object, flags;
            if (linuxFilesSocketPeek(entry->fd, &handle, &flags)) {
                native[sockets] = (LinuxNetPollEntry){handle, (int16_t)linuxNetPollEventsToNative((int)entry->wanted), 0};
                index_of[sockets++] = i;
            } else if (linuxFilesVirtualPeek(entry->fd, &kind, &object, &flags)) {
                in_memory_waiter = true;
                entry->ready = linuxVfdReady(kind, object, flags & 3, entry->wanted);
            } else if (linuxFilesIsOpen(entry->fd))
                entry->ready = entry->wanted & (LINUX_POLLIN | LINUX_POLLOUT);  // files and devices never block
            else
                entry->ready = LINUX_POLLNVAL;
            if (entry->ready) ++ready_count;
        }
        int remaining = timeout_ms < 0 ? -1 : (int)(timeout_ms - (int)(nowMs() - start));
        if (timeout_ms >= 0 && remaining < 0) remaining = 0;
        int slice = ready_count ? 0 : remaining;
        if (!ready_count && in_memory_waiter && (slice < 0 || slice > WAIT_SLICE_MS))
            slice = WAIT_SLICE_MS;  // an in-memory descriptor can become ready at any time
        if (sockets) {
            int result = linuxNetNativePoll(native, sockets, slice, error);
            if (result < 0) return -1;
            for (unsigned i = 0; i < sockets; ++i) {
                LinuxVfdWait *entry = &entries[index_of[i]];
                entry->ready = (unsigned)linuxNetPollEventsFromNative(native[i].revents);
                if (entry->ready) ++ready_count;
            }
        } else if (!ready_count && slice != 0) {
            if (slice < 0) slice = WAIT_SLICE_MS;
            sleepMs((unsigned)slice);
        }
        if (ready_count) return (int)ready_count;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && (int64_t)(nowMs() - start) >= timeout_ms) return 0;
    }
}

// ---- guest ABI: eventfd and pipes ----------------------------------------------------------------------------------------------------
int linuxAbiEventfd(unsigned initial, int flags) {
    if (flags & ~(O_NONBLOCK_FLAG | O_CLOEXEC_FLAG | EFD_SEMAPHORE)) return fail(LINUX_EINVAL);
    int object = createEventfd(initial, (flags & EFD_SEMAPHORE) != 0);
    if (object < 0) return fail(LINUX_EMFILE);
    int fd = linuxFilesVirtualCreate(LINUX_VFD_EVENTFD, object, 2 | (flags & O_NONBLOCK_FLAG));
    if (fd < 0) {
        linuxVfdRelease(LINUX_VFD_EVENTFD, object, 0);
        return -1;
    }
    return fd;
}
int linuxAbiPipe2(int descriptors[2], int flags) {
    if (!descriptors) return fail(LINUX_EFAULT);
    if (flags & ~(O_NONBLOCK_FLAG | O_CLOEXEC_FLAG)) return fail(LINUX_EINVAL);
    linuxSyncLock();
    int object = -1;
    for (unsigned i = 0; i < MAX_OBJECTS && object < 0; ++i)
        if (!pipes[i].used) object = (int)i;
    unsigned char *data = object >= 0 ? malloc(PIPE_BYTES) : NULL;
    if (object >= 0 && data) pipes[object] = (Pipe){.used = true, .read_open = true, .write_open = true, .data = data};
    linuxSyncUnlock();
    if (object < 0 || !data) {
        free(data);
        return fail(LINUX_EMFILE);
    }
    int reader = linuxFilesVirtualCreate(LINUX_VFD_PIPE, object, 0 | (flags & O_NONBLOCK_FLAG));
    if (reader < 0) {
        linuxVfdRelease(LINUX_VFD_PIPE, object, 0);
        linuxVfdRelease(LINUX_VFD_PIPE, object, 1);
        return -1;
    }
    int writer = linuxFilesVirtualCreate(LINUX_VFD_PIPE, object, 1 | (flags & O_NONBLOCK_FLAG));
    if (writer < 0) {
        linuxAbiClose(reader);
        linuxVfdRelease(LINUX_VFD_PIPE, object, 1);
        return -1;
    }
    descriptors[0] = reader;
    descriptors[1] = writer;
    return 0;
}
int linuxAbiPipe(int descriptors[2]) { return linuxAbiPipe2(descriptors, 0); }

// ---- guest ABI: epoll -----------------------------------------------------------------------------------------------------------------
int linuxAbiEpollCreate1(int flags) {
    if (flags & ~O_CLOEXEC_FLAG) return fail(LINUX_EINVAL);
    linuxSyncLock();
    int object = -1;
    for (unsigned i = 0; i < MAX_OBJECTS && object < 0; ++i)
        if (!epolls[i].used) object = (int)i;
    if (object >= 0) epolls[object] = (Epoll){.used = true};
    linuxSyncUnlock();
    if (object < 0) return fail(LINUX_EMFILE);
    int fd = linuxFilesVirtualCreate(LINUX_VFD_EPOLL, object, 2);
    if (fd < 0) {
        linuxVfdRelease(LINUX_VFD_EPOLL, object, 0);
        return -1;
    }
    return fd;
}
int linuxAbiEpollCreate(int size) { return size <= 0 ? fail(LINUX_EINVAL) : linuxAbiEpollCreate1(0); }

static bool epollObject(int fd, int *object) {
    int kind, flags;
    if (!linuxFilesVirtualPeek(fd, &kind, object, &flags)) return false;
    return kind == LINUX_VFD_EPOLL;
}
int linuxAbiEpollCtl(int epoll_fd, int operation, int fd, const LinuxEpollEvent *event) {
    int object;
    if (!epollObject(epoll_fd, &object)) return fail(linuxFilesIsOpen(epoll_fd) ? LINUX_EINVAL : LINUX_EBADF);
    if (!linuxFilesIsOpen(fd)) return fail(LINUX_EBADF);
    if (fd == epoll_fd) return fail(LINUX_EINVAL);
    if (operation != EPOLL_CTL_DEL && !event) return fail(LINUX_EFAULT);
    int error = 0;
    linuxSyncLock();
    Epoll *epoll = &epolls[object];
    int found = -1;
    for (unsigned i = 0; i < epoll->count; ++i)
        if (epoll->interest[i].fd == fd) found = (int)i;
    if (operation == EPOLL_CTL_ADD) {
        if (found >= 0)
            error = LINUX_EEXIST;
        else if (epoll->count >= MAX_INTEREST)
            error = LINUX_ENOMEM;
        else
            epoll->interest[epoll->count++] = (Interest){fd, event->events, event->data};
    } else if (operation == EPOLL_CTL_MOD) {
        if (found < 0)
            error = LINUX_ENOENT;
        else
            epoll->interest[found] = (Interest){fd, event->events, event->data};
    } else if (operation == EPOLL_CTL_DEL) {
        if (found < 0)
            error = LINUX_ENOENT;
        else
            epoll->interest[found] = epoll->interest[--epoll->count];
    } else
        error = LINUX_EINVAL;
    linuxSyncUnlock();
    return error ? fail(error) : 0;
}
int linuxAbiEpollWait(int epoll_fd, LinuxEpollEvent *events, int max, int timeout_ms) {
    int object;
    if (!epollObject(epoll_fd, &object)) return fail(linuxFilesIsOpen(epoll_fd) ? LINUX_EINVAL : LINUX_EBADF);
    if (max <= 0) return fail(LINUX_EINVAL);
    if (!events) return fail(LINUX_EFAULT);
    Interest snapshot[MAX_INTEREST];
    linuxSyncLock();
    unsigned count = epolls[object].count;
    memcpy(snapshot, epolls[object].interest, count * sizeof(Interest));
    linuxSyncUnlock();
    LinuxVfdWait waiting[MAX_INTEREST];
    for (unsigned i = 0; i < count; ++i) {
        unsigned wanted = 0;
        if (snapshot[i].events & EPOLLIN) wanted |= LINUX_POLLIN;
        if (snapshot[i].events & EPOLLOUT) wanted |= LINUX_POLLOUT;
        waiting[i] = (LinuxVfdWait){snapshot[i].fd, wanted, 0};
    }
    int error = 0;
    int ready = linuxVfdWait(waiting, count, timeout_ms, &error);
    if (ready < 0) return fail(error);
    int produced = 0;
    for (unsigned i = 0; i < count && produced < max; ++i) {
        if (!waiting[i].ready || (waiting[i].ready & LINUX_POLLNVAL)) continue;  // a closed descriptor silently leaves the set
        uint32_t bits = 0;
        if (waiting[i].ready & LINUX_POLLIN) bits |= EPOLLIN;
        if (waiting[i].ready & LINUX_POLLOUT) bits |= EPOLLOUT;
        if (waiting[i].ready & LINUX_POLLERR) bits |= EPOLLERR;
        if (waiting[i].ready & LINUX_POLLHUP) bits |= EPOLLHUP;
        events[produced++] = (LinuxEpollEvent){bits, 0, snapshot[i].data};
    }
    return produced;
}
int linuxAbiEpollPwait(int epoll_fd, LinuxEpollEvent *events, int max, int timeout_ms, const void *sigmask) {
    (void)sigmask;
    return linuxAbiEpollWait(epoll_fd, events, max, timeout_ms);
}
