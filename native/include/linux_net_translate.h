#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Translation between the Linux AArch64 socket ABI and Horizon's BSD socket service. Pure functions.

// Linux numbers used by the guest.
enum {
    LINUX_AF_UNSPEC = 0,
    LINUX_AF_INET = 2,
    LINUX_AF_INET6 = 10,
    LINUX_SOCK_STREAM = 1,
    LINUX_SOCK_DGRAM = 2,
    LINUX_SOCK_NONBLOCK = 0x800,
    LINUX_SOCK_CLOEXEC = 0x80000,
    LINUX_SOL_SOCKET = 1,
    LINUX_IPPROTO_TCP = 6,
    LINUX_SO_REUSEADDR = 2,
    LINUX_SO_TYPE = 3,
    LINUX_SO_ERROR = 4,
    LINUX_SO_BROADCAST = 6,
    LINUX_SO_SNDBUF = 7,
    LINUX_SO_RCVBUF = 8,
    LINUX_SO_KEEPALIVE = 9,
    LINUX_SO_LINGER = 13,
    LINUX_SO_REUSEPORT = 15,
    LINUX_SO_RCVTIMEO = 20,
    LINUX_SO_SNDTIMEO = 21,
    LINUX_TCP_NODELAY = 1,
    LINUX_MSG_PEEK = 2,
    LINUX_MSG_DONTWAIT = 0x40,
    LINUX_MSG_WAITALL = 0x100,
    LINUX_POLLIN = 1,
    LINUX_POLLPRI = 2,
    LINUX_POLLOUT = 4,
    LINUX_POLLERR = 8,
    LINUX_POLLHUP = 0x10,
    LINUX_POLLNVAL = 0x20,
    LINUX_AI_PASSIVE = 1,
    LINUX_AI_CANONNAME = 2,
    LINUX_AI_NUMERICHOST = 4,
    LINUX_AI_NUMERICSERV = 0x400,
    LINUX_EAI_BADFLAGS = -1,
    LINUX_EAI_NONAME = -2,
    LINUX_EAI_AGAIN = -3,
    LINUX_EAI_FAIL = -4,
    LINUX_EAI_FAMILY = -6,
    LINUX_EAI_SOCKTYPE = -7,
    LINUX_EAI_SERVICE = -8,
    LINUX_EAI_MEMORY = -10,
    LINUX_EAI_SYSTEM = -11,
    LINUX_EAI_OVERFLOW = -12
};

// Linux error numbers the network code needs beyond those of linux_abi.h.
enum { LINUX_EPIPE = 32, LINUX_ENOPROTOOPT = 92, LINUX_EPROTONOSUPPORT = 93, LINUX_EAFNOSUPPORT = 97 };

// Linux and Horizon layouts of an IPv4 address.
typedef struct {
    uint16_t family, port;
    uint32_t address;
    uint8_t zero[8];
} LinuxSockaddrIn;  // port and address in network order
typedef struct {
    uint8_t length, family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
} BsdSockaddrIn;
_Static_assert(sizeof(LinuxSockaddrIn) == 16 && sizeof(BsdSockaddrIn) == 16, "sockaddr_in layouts");

// Horizon BSD constants (checked against libnx's headers in the platform layer).
enum {
    BSD_AF_INET = 2,
    BSD_SOCK_STREAM = 1,
    BSD_SOCK_DGRAM = 2,
    BSD_SOL_SOCKET = 0xffff,
    BSD_IPPROTO_TCP = 6,
    BSD_SO_REUSEADDR = 0x0004,
    BSD_SO_KEEPALIVE = 0x0008,
    BSD_SO_BROADCAST = 0x0020,
    BSD_SO_LINGER = 0x0080,
    BSD_SO_REUSEPORT = 0x0200,
    BSD_SO_SNDBUF = 0x1001,
    BSD_SO_RCVBUF = 0x1002,
    BSD_SO_SNDTIMEO = 0x1005,
    BSD_SO_RCVTIMEO = 0x1006,
    BSD_SO_ERROR = 0x1007,
    BSD_SO_TYPE = 0x1008,
    BSD_TCP_NODELAY = 1,
    BSD_MSG_PEEK = 0x02,
    BSD_MSG_DONTWAIT = 0x80,
    BSD_MSG_WAITALL = 0x40,
    BSD_POLLIN = 0x01,
    BSD_POLLPRI = 0x02,
    BSD_POLLOUT = 0x04,
    BSD_POLLERR = 0x08,
    BSD_POLLHUP = 0x10,
    BSD_POLLNVAL = 0x20,
    BSD_EAI_NONAME = 8,
    BSD_EAI_AGAIN = 2,
    BSD_EAI_FAIL = 4,
    BSD_EAI_FAMILY = 5,
    BSD_EAI_SERVICE = 9,
    BSD_EAI_MEMORY = 6,
    BSD_EAI_BADFLAGS = 3,
    BSD_EAI_SOCKTYPE = 10
};

// Errors of the platform layer are already Linux numbers; this maps the numbers of newlib (libnx's C library).
int linuxNetErrnoFromNative(int native_errno);

// sockaddr: 0 or a Linux errno. Only AF_INET exists; anything else is EAFNOSUPPORT.
int linuxNetSockaddrToNative(const void *guest, unsigned guest_length, BsdSockaddrIn *native);
// Fills the guest buffer like getsockname/accept do: copies at most *length bytes and stores the full length (16).
void linuxNetSockaddrFromNative(const BsdSockaddrIn *native, void *guest, unsigned *length);

// Socket options: translate (level, name) to the BSD pair; false when the option is not supported.
bool linuxNetOptionToNative(int level, int name, int *native_level, int *native_name);
int linuxNetMessageFlagsToNative(int linux_flags);
int linuxNetPollEventsToNative(int events);
int linuxNetPollEventsFromNative(int events);
int linuxNetEaiFromNative(int native_code);  // BSD EAI_* to the glibc number
const char *linuxNetGaiStrerror(int code);   // text for a glibc EAI_* number
