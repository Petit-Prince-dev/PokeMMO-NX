#include "linux_net.h"
#include "diagnostics.h"
#include "linux_abi.h"
#include "linux_files.h"
#include "linux_net_native.h"
#include "linux_vfd.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_uint sockets_created, connects, polls, resolves, failures;
static atomic_uint traced_lines;
static void trace(const char *format, ...) {
    if (atomic_fetch_add(&traced_lines, 1) >= 300) return;
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}
static int fail(int error) {
    atomic_fetch_add(&failures, 1);
    *linuxAbiErrnoLocation() = error;
    return -1;
}
static int64_t fail64(int error) {
    fail(error);
    return -1;
}
LinuxNetStats linuxNetStats(void) {
    return (LinuxNetStats){atomic_load(&sockets_created), linuxFilesSocketCount(), atomic_load(&connects),
                           atomic_load(&polls),           atomic_load(&resolves),  atomic_load(&failures)};
}

enum { NONBLOCK = 0x800 };  // O_NONBLOCK and SOCK_NONBLOCK are both 04000 on Linux AArch64

int linuxAbiSocket(int domain, int type, int protocol) {
    if (domain != LINUX_AF_INET) {
        trace("net.socket=REFUSED domain=%d", domain);
        return fail(domain == LINUX_AF_INET6 ? LINUX_EAFNOSUPPORT : LINUX_EAFNOSUPPORT);
    }
    int flags = type & (LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
    int kind = type & ~(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
    if (kind != LINUX_SOCK_STREAM && kind != LINUX_SOCK_DGRAM) return fail(LINUX_EPROTONOSUPPORT);
    int error = 0;
    int handle = linuxNetNativeSocket(kind, protocol, &error);
    if (handle < 0) return fail(error);
    if ((flags & LINUX_SOCK_NONBLOCK) && linuxNetNativeSetNonblocking(handle, true, &error) < 0) {
        linuxNetNativeClose(handle);
        return fail(error);
    }
    int fd = linuxFilesSocketCreate(handle, 2 /* O_RDWR */ | ((flags & LINUX_SOCK_NONBLOCK) ? NONBLOCK : 0));
    if (fd < 0) {
        linuxNetNativeClose(handle);
        atomic_fetch_add(&failures, 1);
        return -1;
    }
    atomic_fetch_add(&sockets_created, 1);
    return fd;
}

static bool lookup(int fd, int *handle) {
    int flags = 0;
    return linuxFilesSocketLookup(fd, handle, &flags);
}
int linuxAbiConnect(int fd, const void *address, unsigned length) {
    int handle, error = 0;
    if (!lookup(fd, &handle)) return -1;
    BsdSockaddrIn native;
    if ((error = linuxNetSockaddrToNative(address, length, &native))) return fail(error);
    atomic_fetch_add(&connects, 1);
    if (linuxNetNativeConnect(handle, &native, &error) < 0) return fail(error);
    return 0;
}
int linuxAbiBind(int fd, const void *address, unsigned length) {
    int handle, error = 0;
    if (!lookup(fd, &handle)) return -1;
    BsdSockaddrIn native;
    if ((error = linuxNetSockaddrToNative(address, length, &native))) return fail(error);
    return linuxNetNativeBind(handle, &native, &error) < 0 ? fail(error) : 0;
}
int linuxAbiListen(int fd, int backlog) {
    int handle, error = 0;
    if (!lookup(fd, &handle)) return -1;
    return linuxNetNativeListen(handle, backlog, &error) < 0 ? fail(error) : 0;
}
int linuxAbiAccept4(int fd, void *address, unsigned *length, int flags) {
    int handle, error = 0;
    if (!lookup(fd, &handle)) return -1;
    BsdSockaddrIn peer;
    memset(&peer, 0, sizeof(peer));
    int accepted = linuxNetNativeAccept(handle, &peer, &error);
    if (accepted < 0) return fail(error);
    if ((flags & LINUX_SOCK_NONBLOCK) && linuxNetNativeSetNonblocking(accepted, true, &error) < 0) {
        linuxNetNativeClose(accepted);
        return fail(error);
    }
    int guest = linuxFilesSocketCreate(accepted, 2 | ((flags & LINUX_SOCK_NONBLOCK) ? NONBLOCK : 0));
    if (guest < 0) {
        linuxNetNativeClose(accepted);
        return -1;
    }
    if (address && length) linuxNetSockaddrFromNative(&peer, address, length);
    atomic_fetch_add(&sockets_created, 1);
    return guest;
}
int linuxAbiAccept(int fd, void *address, unsigned *length) { return linuxAbiAccept4(fd, address, length, 0); }
static int nameOf(int fd, void *address, unsigned *length, bool peer) {
    int handle, error = 0;
    if (!lookup(fd, &handle)) return -1;
    if (!address || !length) return fail(LINUX_EFAULT);
    BsdSockaddrIn native;
    memset(&native, 0, sizeof(native));
    if (linuxNetNativeName(handle, peer, &native, &error) < 0) return fail(error);
    native.family = BSD_AF_INET;
    linuxNetSockaddrFromNative(&native, address, length);
    return 0;
}
int linuxAbiGetsockname(int fd, void *address, unsigned *length) { return nameOf(fd, address, length, false); }
int linuxAbiGetpeername(int fd, void *address, unsigned *length) { return nameOf(fd, address, length, true); }

// SO_ERROR comes back with Horizon's own errno numbers; the guest wants Linux's.
static int linuxErrorFromHorizon(int value) {
    switch (value) {
        case 0: return 0;
        case 32: return 32;   // EPIPE
        case 35: return 11;   // EWOULDBLOCK
        case 36: return 115;  // EINPROGRESS
        case 37: return 114;  // EALREADY
        case 48: return 98;   // EADDRINUSE
        case 49: return 99;   // EADDRNOTAVAIL
        case 50: return 100;  // ENETDOWN
        case 51: return 101;  // ENETUNREACH
        case 53: return 103;  // ECONNABORTED
        case 54: return 104;  // ECONNRESET
        case 56: return 106;  // EISCONN
        case 57: return 107;  // ENOTCONN
        case 60: return 110;  // ETIMEDOUT
        case 61: return 111;  // ECONNREFUSED
        case 65: return 113;  // EHOSTUNREACH
        default: return value >= 100 ? linuxNetErrnoFromNative(value) : LINUX_EIO;
    }
}
int linuxAbiSetsockopt(int fd, int level, int name, const void *value, unsigned length) {
    int handle, error = 0, native_level, native_name;
    if (!lookup(fd, &handle)) return -1;
    if (level == LINUX_SOL_SOCKET && (name == LINUX_SO_RCVTIMEO || name == LINUX_SO_SNDTIMEO)) return 0;  // timeouts are done with poll
    if (!linuxNetOptionToNative(level, name, &native_level, &native_name)) {
        trace("net.setsockopt=UNSUPPORTED level=%d name=%d", level, name);
        return fail(LINUX_ENOPROTOOPT);
    }
    if (!value && length) return fail(LINUX_EFAULT);
    return linuxNetNativeSetsockopt(handle, native_level, native_name, value, length, &error) < 0 ? fail(error) : 0;
}
int linuxAbiGetsockopt(int fd, int level, int name, void *value, unsigned *length) {
    int handle, error = 0, native_level, native_name;
    if (!lookup(fd, &handle)) return -1;
    if (!value || !length) return fail(LINUX_EFAULT);
    if (!linuxNetOptionToNative(level, name, &native_level, &native_name)) {
        trace("net.getsockopt=UNSUPPORTED level=%d name=%d", level, name);
        return fail(LINUX_ENOPROTOOPT);
    }
    if (linuxNetNativeGetsockopt(handle, native_level, native_name, value, length, &error) < 0) return fail(error);
    if (level == LINUX_SOL_SOCKET && name == LINUX_SO_ERROR && *length >= sizeof(int)) {
        int raw;
        memcpy(&raw, value, sizeof(raw));
        int translated = linuxErrorFromHorizon(raw);
        if (raw) trace("net.so_error raw=%d linux=%d", raw, translated);
        memcpy(value, &translated, sizeof(translated));
    }
    return 0;
}
int linuxAbiShutdown(int fd, int how) {
    int handle, error = 0;
    if (!lookup(fd, &handle)) return -1;
    if (how < 0 || how > 2) return fail(LINUX_EINVAL);
    return linuxNetNativeShutdown(handle, how, &error) < 0 ? fail(error) : 0;
}

int64_t linuxAbiSendto(int fd, const void *buffer, size_t count, int flags, const void *address, unsigned length) {
    int handle, error = 0;
    if (!lookup(fd, &handle)) return -1;
    if (count && !buffer) return fail64(LINUX_EFAULT);
    BsdSockaddrIn native;
    if (address && (error = linuxNetSockaddrToNative(address, length, &native))) return fail64(error);
    int64_t sent = linuxNetNativeSend(handle, buffer, count, linuxNetMessageFlagsToNative(flags), address ? &native : NULL, &error);
    if (sent < 0) return fail64(error);
    return sent;
}
int64_t linuxAbiSend(int fd, const void *buffer, size_t count, int flags) { return linuxAbiSendto(fd, buffer, count, flags, NULL, 0); }
int64_t linuxAbiRecvfrom(int fd, void *buffer, size_t count, int flags, void *address, unsigned *length) {
    int handle, error = 0;
    if (!lookup(fd, &handle)) return -1;
    if (count && !buffer) return fail64(LINUX_EFAULT);
    BsdSockaddrIn from;
    memset(&from, 0, sizeof(from));
    int64_t received = linuxNetNativeRecv(handle, buffer, count, linuxNetMessageFlagsToNative(flags), address ? &from : NULL, &error);
    if (received < 0) return fail64(error);
    if (address && length) {
        from.family = BSD_AF_INET;
        linuxNetSockaddrFromNative(&from, address, length);
    }
    return received;
}
int64_t linuxAbiRecv(int fd, void *buffer, size_t count, int flags) { return linuxAbiRecvfrom(fd, buffer, count, flags, NULL, NULL); }

// poll(2): the common waiter of linux_vfd.c (sockets wait in the BSD service, in-memory descriptors are watched in slices).
int linuxAbiPoll(LinuxPollfd *fds, unsigned long count, int timeout_ms) {
    if (count && !fds) return fail(LINUX_EFAULT);
    if (count > 64) return fail(LINUX_EINVAL);
    LinuxVfdWait waiting[64];
    atomic_fetch_add(&polls, 1);
    for (unsigned long i = 0; i < count; ++i) waiting[i] = (LinuxVfdWait){fds[i].fd, (unsigned)fds[i].events, 0};
    int error = 0;
    int ready = linuxVfdWait(waiting, (unsigned)count, timeout_ms, &error);
    if (ready < 0) return fail(error);
    for (unsigned long i = 0; i < count; ++i) fds[i].revents = (int16_t)waiting[i].ready;
    return ready;
}

// The interfaces Java's NetworkInterface asks about (SIOCGIF* on a socket): the loopback and, when the console has a network, its Wi-Fi.
typedef struct {
    char name[16];
    union {
        LinuxSockaddrIn address;
        int16_t flags;
        int32_t value;
        struct {
            uint16_t family;
            uint8_t data[14];
        } hardware;
        uint8_t raw[24];
    } u;
} LinuxIfreq;
_Static_assert(sizeof(LinuxIfreq) == 40, "ifreq layout");
typedef struct {
    int32_t length, pad;
    LinuxIfreq *buffer;
} LinuxIfconf;
typedef struct {
    const char *name;
    uint32_t address, netmask;
    int16_t flags;
    int32_t mtu, index;
    uint16_t hardware_family;
} LinuxInterface;
static unsigned interfaces(LinuxInterface *list) {
    static const uint8_t loop[4] = {127, 0, 0, 1}, loop_mask[4] = {255, 0, 0, 0};
    LinuxInterface lo = {"lo", 0, 0, 0x1 | 0x8 | 0x40, 65536, 1, 772};  // UP LOOPBACK RUNNING
    memcpy(&lo.address, loop, 4);
    memcpy(&lo.netmask, loop_mask, 4);
    list[0] = lo;
    uint32_t address, netmask;
    if (!linuxNetNativeLocalNetwork(&address, &netmask)) return 1;
    list[1] = (LinuxInterface){"wlan0", address, netmask, 0x1 | 0x2 | 0x40 | 0x1000, 1500, 2, 1};  // UP BROADCAST RUNNING MULTICAST
    return 2;
}
static int interfaceRequest(unsigned long request, void *argument) {
    enum {
        SIOCGIFCONF = 0x8912,
        SIOCGIFFLAGS = 0x8913,
        SIOCGIFADDR = 0x8915,
        SIOCGIFBRDADDR = 0x8919,
        SIOCGIFNETMASK = 0x891b,
        SIOCGIFMTU = 0x8921,
        SIOCGIFHWADDR = 0x8927,
        SIOCGIFINDEX = 0x8933,
        NO_SUCH_INTERFACE = 19,
        NO_BROADCAST = 99
    };
    if (!argument) return fail(LINUX_EFAULT);
    LinuxInterface list[2];
    unsigned count = interfaces(list);
    if (request == SIOCGIFCONF) {
        LinuxIfconf *conf = argument;
        if (!conf->buffer) {
            conf->length = (int32_t)(count * sizeof(LinuxIfreq));
            return 0;
        }
        unsigned fit = conf->length > 0 ? (unsigned)conf->length / (unsigned)sizeof(LinuxIfreq) : 0, written = fit < count ? fit : count;
        for (unsigned i = 0; i < written; ++i) {
            memset(&conf->buffer[i], 0, sizeof(LinuxIfreq));
            strncpy(conf->buffer[i].name, list[i].name, sizeof(conf->buffer[i].name) - 1);
            conf->buffer[i].u.address = (LinuxSockaddrIn){LINUX_AF_INET, 0, list[i].address, {0}};
        }
        conf->length = (int32_t)(written * sizeof(LinuxIfreq));
        return 0;
    }
    LinuxIfreq *item = argument;
    const LinuxInterface *found = NULL;
    for (unsigned i = 0; i < count; ++i)
        if (!strncmp(item->name, list[i].name, sizeof(item->name))) found = &list[i];
    if (!found) return fail(NO_SUCH_INTERFACE);
    memset(&item->u, 0, sizeof(item->u));
    switch (request) {
        case SIOCGIFFLAGS: item->u.flags = found->flags; return 0;
        case SIOCGIFADDR: item->u.address = (LinuxSockaddrIn){LINUX_AF_INET, 0, found->address, {0}}; return 0;
        case SIOCGIFNETMASK: item->u.address = (LinuxSockaddrIn){LINUX_AF_INET, 0, found->netmask, {0}}; return 0;
        case SIOCGIFBRDADDR:
            if (!(found->flags & 0x2)) return fail(NO_BROADCAST);
            item->u.address = (LinuxSockaddrIn){LINUX_AF_INET, 0, found->address | ~found->netmask, {0}};
            return 0;
        case SIOCGIFMTU: item->u.value = found->mtu; return 0;
        case SIOCGIFINDEX: item->u.value = found->index; return 0;
        case SIOCGIFHWADDR: item->u.hardware.family = found->hardware_family; return 0;  // no hardware address is told: all zeros
        default: return fail(25);
    }
}
static bool isInterfaceRequest(unsigned long request) {
    return request == 0x8912 || request == 0x8913 || request == 0x8915 || request == 0x8919 || request == 0x891b || request == 0x8921 || request == 0x8927 ||
           request == 0x8933;
}

int linuxAbiIoctl(int fd, unsigned long request, ...) {
    enum { FIONBIO = 0x5421, FIONREAD = 0x541b };
    va_list args;
    va_start(args, request);
    void *argument = va_arg(args, void *);
    va_end(args);
    int handle, flags = 0, error = 0;
    if (!linuxFilesSocketPeek(fd, &handle, &flags)) {
        if (!linuxFilesIsOpen(fd)) return fail(LINUX_EBADF);
        return fail(25);  // ENOTTY: not a terminal, nothing else is understood
    }
    switch (request) {
        case FIONBIO: {
            bool enable = argument && *(const int *)argument;
            if (linuxNetNativeSetNonblocking(handle, enable, &error) < 0) return fail(error);
            linuxFilesSocketSetNonblocking(fd, enable);
            return 0;
        }
        case FIONREAD: {
            if (!argument) return fail(LINUX_EFAULT);
            // Peek into a small buffer: enough for a yes/no, which is what callers need.
            unsigned char scratch[1024];
            int64_t peeked = linuxNetNativeRecv(handle, scratch, sizeof(scratch), BSD_MSG_PEEK | BSD_MSG_DONTWAIT, NULL, &error);
            *(int *)argument = peeked > 0 ? (int)peeked : 0;
            return 0;
        }
        default: return isInterfaceRequest(request) ? interfaceRequest(request, argument) : fail(25);
    }
}

// The console answers to the name it gives itself (gethostname): the game's Java asks for it through InetAddress.getLocalHost.
static bool isOwnName(const char *node) {
    static const char own[] = "switch";
    for (unsigned i = 0; i < sizeof(own); ++i) {
        char c = node[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != own[i]) return false;
    }
    return true;
}
static int resolveOwnName(const char *service, LinuxNetResolved *result) {
    memset(result, 0, sizeof(*result));
    uint32_t address = 0, netmask = 0;
    if (!linuxNetNativeLocalNetwork(&address, &netmask)) {
        static const uint8_t loop[4] = {127, 0, 0, 1};
        memcpy(&address, loop, 4);
    }
    uint16_t port = 0;
    if (service) {
        unsigned long value = 0;
        if (!*service) return LINUX_EAI_SERVICE;
        for (const char *p = service; *p; ++p) {
            if (*p < '0' || *p > '9' || value > 65535) return LINUX_EAI_SERVICE;
            value = value * 10 + (unsigned long)(*p - '0');
        }
        if (value > 65535) return LINUX_EAI_SERVICE;
        port = (uint16_t)((value & 255) << 8 | (value >> 8));
    }
    result->count = 1;
    result->entries[0].address = address;
    result->entries[0].port = port;
    strcpy(result->canonical_name, "switch");
    return 0;
}

// getaddrinfo(3): one allocation per result (structure, address and canonical name together), released by freeaddrinfo.
int linuxAbiGetaddrinfo(const char *node, const char *service, const LinuxAddrinfo *hints, LinuxAddrinfo **result) {
    if (!result) return LINUX_EAI_SYSTEM;
    *result = NULL;
    if (!node && !service) return LINUX_EAI_NONAME;
    int flags = hints ? hints->flags : 0, family = hints ? hints->family : LINUX_AF_UNSPEC;
    int socktype = hints ? hints->socktype : 0, protocol = hints ? hints->protocol : 0;
    atomic_fetch_add(&resolves, 1);
    if (family != LINUX_AF_UNSPEC && family != LINUX_AF_INET) return family == LINUX_AF_INET6 ? LINUX_EAI_NONAME : LINUX_EAI_FAMILY;
    if (socktype != 0 && socktype != LINUX_SOCK_STREAM && socktype != LINUX_SOCK_DGRAM) return LINUX_EAI_SOCKTYPE;
    LinuxNetResolved resolved;
    int code = node && !(flags & LINUX_AI_NUMERICHOST) && isOwnName(node) ? resolveOwnName(service, &resolved)
                                                                          : linuxNetNativeResolve(node, service, flags, socktype, protocol, &resolved);
    if (code) return code;
    LinuxAddrinfo *head = NULL, **tail = &head;
    for (unsigned i = 0; i < resolved.count; ++i) {
        int entry_type = resolved.entries[i].socktype;
        if (socktype && entry_type && entry_type != socktype) continue;
        bool want_name = (flags & LINUX_AI_CANONNAME) && !head;
        size_t name_size = want_name ? strlen(resolved.canonical_name[0] ? resolved.canonical_name : (node ? node : "")) + 1 : 0;
        size_t total = sizeof(LinuxAddrinfo) + sizeof(LinuxSockaddrIn) + name_size;
        LinuxAddrinfo *item = calloc(1, total);
        if (!item) {
            linuxAbiFreeaddrinfo(head);
            return LINUX_EAI_MEMORY;
        }
        LinuxSockaddrIn *address = (LinuxSockaddrIn *)(item + 1);
        address->family = LINUX_AF_INET;
        address->port = resolved.entries[i].port;
        address->address = resolved.entries[i].address;
        item->flags = flags;
        item->family = LINUX_AF_INET;
        item->socktype = entry_type ? entry_type : (socktype ? socktype : LINUX_SOCK_STREAM);
        item->protocol = resolved.entries[i].protocol ? resolved.entries[i].protocol : protocol;
        item->address_length = sizeof(LinuxSockaddrIn);
        item->address = address;
        if (want_name) {
            item->canonical_name = (char *)(address + 1);
            strcpy(item->canonical_name, resolved.canonical_name[0] ? resolved.canonical_name : (node ? node : ""));
        }
        *tail = item;
        tail = (LinuxAddrinfo **)&item->next;
    }
    if (!head) return LINUX_EAI_NONAME;
    *result = head;
    return 0;
}
void linuxAbiFreeaddrinfo(LinuxAddrinfo *list) {
    while (list) {
        LinuxAddrinfo *next = list->next;
        free(list);
        list = next;
    }
}
const char *linuxAbiGaiStrerror(int code) { return linuxNetGaiStrerror(code); }

int linuxAbiGethostname(char *name, size_t length) {
    static const char host[] = "switch";
    if (!name) return fail(LINUX_EFAULT);
    if (length < sizeof(host)) return fail(36);  // ENAMETOOLONG
    memcpy(name, host, sizeof(host));
    return 0;
}
int linuxAbiInetPton(int family, const char *text, void *destination) {
    if (family != LINUX_AF_INET) {
        *linuxAbiErrnoLocation() = LINUX_EAFNOSUPPORT;
        return -1;
    }
    if (!text || !destination) return 0;
    unsigned char bytes[4];
    unsigned parts = 0, value = 0, digits = 0;
    for (const char *p = text;; ++p) {
        if (*p >= '0' && *p <= '9') {
            if (digits && !value) return 0;  // a leading zero is not accepted
            value = value * 10 + (unsigned)(*p - '0');
            if (++digits > 3 || value > 255) return 0;
        } else if (*p == '.' || *p == 0) {
            if (!digits || parts >= 4) return 0;
            bytes[parts++] = (unsigned char)value;
            value = digits = 0;
            if (!*p) break;
        } else
            return 0;
    }
    if (parts != 4) return 0;
    memcpy(destination, bytes, 4);
    return 1;
}
