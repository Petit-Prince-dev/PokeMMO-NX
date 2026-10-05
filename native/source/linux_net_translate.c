#include "linux_net_translate.h"
#include "linux_abi.h"
#include <errno.h>
#include <string.h>

// newlib (libnx's C library) numbers some errors differently from Linux: the native names are used, so errors translate by meaning.
int linuxNetErrnoFromNative(int native_errno) {
    switch (native_errno) {
        case 0: return 0;
        case EPERM: return 1;
        case ENOENT: return 2;
        case EINTR: return 4;
        case EIO: return 5;
        case EBADF: return 9;
        case EAGAIN: return 11;
        case ENOMEM: return 12;
        case EACCES: return 13;
        case EFAULT: return 14;
        case EBUSY: return 16;
        case EINVAL: return 22;
        case EMFILE: return 24;
        case EPIPE: return 32;
        case ENOSYS: return 38;
        case ENOTSOCK: return 88;
        case EDESTADDRREQ: return 89;
        case EMSGSIZE: return 90;
        case EPROTOTYPE: return 91;
        case ENOPROTOOPT: return 92;
        case EPROTONOSUPPORT: return 93;
        case EOPNOTSUPP: return 95;
        case EAFNOSUPPORT: return 97;
        case EADDRINUSE: return 98;
        case EADDRNOTAVAIL: return 99;
        case ENETDOWN: return 100;
        case ENETUNREACH: return 101;
        case ECONNABORTED: return 103;
        case ECONNRESET: return 104;
        case ENOBUFS: return 105;
        case EISCONN: return 106;
        case ENOTCONN: return 107;
        case ETIMEDOUT: return 110;
        case ECONNREFUSED: return 111;
        case EHOSTUNREACH: return 113;
        case EALREADY: return 114;
        case EINPROGRESS: return 115;
        default: return LINUX_EIO;
    }
}

int linuxNetSockaddrToNative(const void *guest, unsigned guest_length, BsdSockaddrIn *native) {
    if (!guest || !native) return LINUX_EFAULT;
    if (guest_length < sizeof(LinuxSockaddrIn)) return LINUX_EINVAL;
    LinuxSockaddrIn linux_address;
    memcpy(&linux_address, guest, sizeof(linux_address));
    if (linux_address.family != LINUX_AF_INET) return LINUX_EAFNOSUPPORT;
    memset(native, 0, sizeof(*native));
    native->length = sizeof(*native);
    native->family = BSD_AF_INET;
    native->port = linux_address.port;
    native->address = linux_address.address;
    return 0;
}

void linuxNetSockaddrFromNative(const BsdSockaddrIn *native, void *guest, unsigned *length) {
    LinuxSockaddrIn linux_address;
    memset(&linux_address, 0, sizeof(linux_address));
    linux_address.family = LINUX_AF_INET;
    linux_address.port = native->port;
    linux_address.address = native->address;
    if (guest && length) {
        unsigned room = *length < sizeof(linux_address) ? *length : (unsigned)sizeof(linux_address);
        memcpy(guest, &linux_address, room);
    }
    if (length) *length = sizeof(linux_address);  // the full size, even when the buffer was smaller (getsockname semantics)
}

bool linuxNetOptionToNative(int level, int name, int *native_level, int *native_name) {
    if (level == LINUX_SOL_SOCKET) {
        *native_level = BSD_SOL_SOCKET;
        switch (name) {
            case LINUX_SO_REUSEADDR: *native_name = BSD_SO_REUSEADDR; return true;
            case LINUX_SO_TYPE: *native_name = BSD_SO_TYPE; return true;
            case LINUX_SO_ERROR: *native_name = BSD_SO_ERROR; return true;
            case LINUX_SO_BROADCAST: *native_name = BSD_SO_BROADCAST; return true;
            case LINUX_SO_SNDBUF: *native_name = BSD_SO_SNDBUF; return true;
            case LINUX_SO_RCVBUF: *native_name = BSD_SO_RCVBUF; return true;
            case LINUX_SO_KEEPALIVE: *native_name = BSD_SO_KEEPALIVE; return true;
            case LINUX_SO_LINGER: *native_name = BSD_SO_LINGER; return true;
            case LINUX_SO_REUSEPORT: *native_name = BSD_SO_REUSEPORT; return true;
            case LINUX_SO_RCVTIMEO: *native_name = BSD_SO_RCVTIMEO; return true;
            case LINUX_SO_SNDTIMEO: *native_name = BSD_SO_SNDTIMEO; return true;
            default: return false;
        }
    }
    if (level == LINUX_IPPROTO_TCP && name == LINUX_TCP_NODELAY) {
        *native_level = BSD_IPPROTO_TCP;
        *native_name = BSD_TCP_NODELAY;
        return true;
    }
    return false;
}

int linuxNetMessageFlagsToNative(int flags) {
    int native = 0;
    if (flags & LINUX_MSG_PEEK) native |= BSD_MSG_PEEK;
    if (flags & LINUX_MSG_DONTWAIT) native |= BSD_MSG_DONTWAIT;
    if (flags & LINUX_MSG_WAITALL) native |= BSD_MSG_WAITALL;
    return native;  // MSG_NOSIGNAL needs no counterpart: no signal is ever delivered
}

int linuxNetPollEventsToNative(int events) {
    int native = 0;
    if (events & LINUX_POLLIN) native |= BSD_POLLIN;
    if (events & LINUX_POLLPRI) native |= BSD_POLLPRI;
    if (events & LINUX_POLLOUT) native |= BSD_POLLOUT;
    if (events & LINUX_POLLERR) native |= BSD_POLLERR;
    if (events & LINUX_POLLHUP) native |= BSD_POLLHUP;
    return native;
}
int linuxNetPollEventsFromNative(int events) {
    int linux_events = 0;
    if (events & BSD_POLLIN) linux_events |= LINUX_POLLIN;
    if (events & BSD_POLLPRI) linux_events |= LINUX_POLLPRI;
    if (events & BSD_POLLOUT) linux_events |= LINUX_POLLOUT;
    if (events & BSD_POLLERR) linux_events |= LINUX_POLLERR;
    if (events & BSD_POLLHUP) linux_events |= LINUX_POLLHUP;
    if (events & BSD_POLLNVAL) linux_events |= LINUX_POLLNVAL;
    return linux_events;
}

int linuxNetEaiFromNative(int code) {
    switch (code) {
        case 0: return 0;
        case BSD_EAI_BADFLAGS: return LINUX_EAI_BADFLAGS;
        case BSD_EAI_NONAME:
        case 7: return LINUX_EAI_NONAME;  // 7 is EAI_NODATA
        case BSD_EAI_AGAIN: return LINUX_EAI_AGAIN;
        case BSD_EAI_FAIL: return LINUX_EAI_FAIL;
        case BSD_EAI_FAMILY: return LINUX_EAI_FAMILY;
        case BSD_EAI_SOCKTYPE: return LINUX_EAI_SOCKTYPE;
        case BSD_EAI_SERVICE: return LINUX_EAI_SERVICE;
        case BSD_EAI_MEMORY: return LINUX_EAI_MEMORY;
        case 11: return LINUX_EAI_SYSTEM;
        case 14: return LINUX_EAI_OVERFLOW;
        default: return LINUX_EAI_FAIL;
    }
}

const char *linuxNetGaiStrerror(int code) {
    switch (code) {
        case 0: return "Success";
        case LINUX_EAI_BADFLAGS: return "Bad value for ai_flags";
        case LINUX_EAI_NONAME: return "Name or service not known";
        case LINUX_EAI_AGAIN: return "Temporary failure in name resolution";
        case LINUX_EAI_FAIL: return "Non-recoverable failure in name resolution";
        case LINUX_EAI_FAMILY: return "ai_family not supported";
        case LINUX_EAI_SOCKTYPE: return "ai_socktype not supported";
        case LINUX_EAI_SERVICE: return "Servname not supported for ai_socktype";
        case LINUX_EAI_MEMORY: return "Memory allocation failure";
        case LINUX_EAI_SYSTEM: return "System error";
        case LINUX_EAI_OVERFLOW: return "Argument buffer overflow";
        default: return "Unknown error";
    }
}
