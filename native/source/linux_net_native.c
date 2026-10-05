#include "linux_net_native.h"
#include "diagnostics.h"
#include "linux_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <switch.h>
#include <unistd.h>

// Horizon BSD service through libnx (socketInitialize is done by the application at start-up).
_Static_assert(SOL_SOCKET == BSD_SOL_SOCKET && SO_REUSEADDR == BSD_SO_REUSEADDR && SO_KEEPALIVE == BSD_SO_KEEPALIVE && SO_BROADCAST == BSD_SO_BROADCAST &&
                   SO_LINGER == BSD_SO_LINGER && SO_REUSEPORT == BSD_SO_REUSEPORT && SO_SNDBUF == BSD_SO_SNDBUF && SO_RCVBUF == BSD_SO_RCVBUF &&
                   SO_SNDTIMEO == BSD_SO_SNDTIMEO && SO_RCVTIMEO == BSD_SO_RCVTIMEO && SO_ERROR == BSD_SO_ERROR && SO_TYPE == BSD_SO_TYPE,
               "BSD socket options");
_Static_assert(AF_INET == BSD_AF_INET && SOCK_STREAM == BSD_SOCK_STREAM && SOCK_DGRAM == BSD_SOCK_DGRAM && IPPROTO_TCP == BSD_IPPROTO_TCP &&
                   TCP_NODELAY == BSD_TCP_NODELAY,
               "BSD socket constants");
_Static_assert(MSG_PEEK == BSD_MSG_PEEK && MSG_DONTWAIT == BSD_MSG_DONTWAIT && MSG_WAITALL == BSD_MSG_WAITALL, "BSD message flags");
_Static_assert(POLLIN == BSD_POLLIN && POLLPRI == BSD_POLLPRI && POLLOUT == BSD_POLLOUT && POLLERR == BSD_POLLERR && POLLHUP == BSD_POLLHUP &&
                   POLLNVAL == BSD_POLLNVAL,
               "BSD poll bits");
_Static_assert(sizeof(struct sockaddr_in) == sizeof(BsdSockaddrIn) && offsetof(struct sockaddr_in, sin_port) == offsetof(BsdSockaddrIn, port) &&
                   offsetof(struct sockaddr_in, sin_addr) == offsetof(BsdSockaddrIn, address),
               "sockaddr_in layout");

static atomic_uint traced;
static void trace(const char *format, ...) {
    if (atomic_fetch_add(&traced, 1) >= 200) return;
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}
static int failure(const char *operation, int *error) {
    int native = errno;
    *error = linuxNetErrnoFromNative(native);
    trace("net.%s=ERROR native_errno=%d linux_errno=%d", operation, native, *error);
    return -1;
}

int linuxNetNativeSocket(int type, int protocol, int *error) {
    int handle = socket(AF_INET, type, protocol);
    trace("net.socket=%s handle=%d type=%d", handle < 0 ? "ERROR" : "OK", handle, type);
    return handle < 0 ? failure("socket", error) : handle;
}
int linuxNetNativeClose(int handle) {
    int result = close(handle);
    trace("net.close handle=%d result=%d", handle, result);
    return result;
}
int linuxNetNativeSetNonblocking(int handle, bool enable, int *error) {
    int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0) return failure("fcntl_get", error);
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(handle, F_SETFL, flags) < 0 ? failure("fcntl_set", error) : 0;
}
int linuxNetNativeConnect(int handle, const BsdSockaddrIn *address, int *error) {
    int result = connect(handle, (const struct sockaddr *)address, sizeof(*address));
    uint32_t host = __builtin_bswap32(address->address);
    trace("net.connect handle=%d address=%u.%u.%u.%u port=%u result=%d native_errno=%d", handle, host >> 24, (host >> 16) & 255, (host >> 8) & 255, host & 255,
          (unsigned)__builtin_bswap16(address->port), result, result < 0 ? errno : 0);
    return result < 0 ? failure("connect", error) : 0;
}
int linuxNetNativeBind(int handle, const BsdSockaddrIn *address, int *error) {
    return bind(handle, (const struct sockaddr *)address, sizeof(*address)) < 0 ? failure("bind", error) : 0;
}
int linuxNetNativeListen(int handle, int backlog, int *error) { return listen(handle, backlog) < 0 ? failure("listen", error) : 0; }
int linuxNetNativeAccept(int handle, BsdSockaddrIn *address, int *error) {
    socklen_t length = sizeof(*address);
    int accepted = accept(handle, (struct sockaddr *)address, &length);
    return accepted < 0 ? failure("accept", error) : accepted;
}
int linuxNetNativeName(int handle, bool peer, BsdSockaddrIn *address, int *error) {
    socklen_t length = sizeof(*address);
    int result = peer ? getpeername(handle, (struct sockaddr *)address, &length) : getsockname(handle, (struct sockaddr *)address, &length);
    return result < 0 ? failure(peer ? "getpeername" : "getsockname", error) : 0;
}
int linuxNetNativeSetsockopt(int handle, int level, int name, const void *value, unsigned length, int *error) {
    int result = setsockopt(handle, level, name, value, length);
    trace("net.setsockopt handle=%d level=0x%x name=0x%x result=%d", handle, level, name, result);
    return result < 0 ? failure("setsockopt", error) : 0;
}
int linuxNetNativeGetsockopt(int handle, int level, int name, void *value, unsigned *length, int *error) {
    socklen_t native_length = *length;
    int result = getsockopt(handle, level, name, value, &native_length);
    if (result < 0) return failure("getsockopt", error);
    *length = native_length;
    return 0;
}
int linuxNetNativeShutdown(int handle, int how, int *error) { return shutdown(handle, how) < 0 ? failure("shutdown", error) : 0; }
int64_t linuxNetNativeSend(int handle, const void *buffer, size_t count, int flags, const BsdSockaddrIn *to, int *error) {
    ssize_t result = to ? sendto(handle, buffer, count, flags, (const struct sockaddr *)to, sizeof(*to)) : send(handle, buffer, count, flags);
    return result < 0 ? failure("send", error) : (int64_t)result;
}
int64_t linuxNetNativeRecv(int handle, void *buffer, size_t count, int flags, BsdSockaddrIn *from, int *error) {
    socklen_t length = sizeof(*from);
    ssize_t result = from ? recvfrom(handle, buffer, count, flags, (struct sockaddr *)from, &length) : recv(handle, buffer, count, flags);
    return result < 0 ? failure("recv", error) : (int64_t)result;
}
int linuxNetNativePoll(LinuxNetPollEntry *entries, unsigned count, int timeout_ms, int *error) {
    if (!count) {
        if (timeout_ms > 0) svcSleepThread((int64_t)timeout_ms * 1000000ll);
        return 0;
    }
    struct pollfd native[64];
    if (count > 64) {
        *error = LINUX_EINVAL;
        return -1;
    }
    for (unsigned i = 0; i < count; ++i) {
        native[i].fd = entries[i].handle;
        native[i].events = entries[i].events;
        native[i].revents = 0;
    }
    int ready = poll(native, count, timeout_ms);
    if (ready < 0) return failure("poll", error);
    for (unsigned i = 0; i < count; ++i) entries[i].revents = native[i].revents;
    return ready;
}

bool linuxNetNativeLocalNetwork(uint32_t *address, uint32_t *netmask) {
    u32 ip = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    if (R_FAILED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gateway, &dns1, &dns2)) || !ip) return false;
    *address = ip;
    *netmask = mask ? mask : 0x00ffffffu;  // 255.255.255.0 when the console does not say
    return true;
}

int linuxNetNativeResolve(const char *node, const char *service, int flags, int socktype, int protocol, LinuxNetResolved *result) {
    memset(result, 0, sizeof(*result));
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;
    if (flags & LINUX_AI_PASSIVE) hints.ai_flags |= AI_PASSIVE;
    if (flags & LINUX_AI_CANONNAME) hints.ai_flags |= AI_CANONNAME;
    if (flags & LINUX_AI_NUMERICHOST) hints.ai_flags |= AI_NUMERICHOST;
    if (flags & LINUX_AI_NUMERICSERV) hints.ai_flags |= AI_NUMERICSERV;
    struct addrinfo *list = NULL;
    int code = getaddrinfo(node, service, &hints, &list);
    trace("net.getaddrinfo node=%s service=%s result=%d", node ? node : "(null)", service ? service : "(null)", code);
    if (code) return linuxNetEaiFromNative(code);
    for (struct addrinfo *item = list; item; item = item->ai_next) {
        if (item->ai_family != AF_INET || !item->ai_addr || result->count >= LINUX_NET_MAX_RESOLVED) continue;
        const struct sockaddr_in *address = (const struct sockaddr_in *)item->ai_addr;
        result->entries[result->count].address = address->sin_addr.s_addr;
        result->entries[result->count].port = address->sin_port;
        result->entries[result->count].socktype = item->ai_socktype;
        result->entries[result->count].protocol = item->ai_protocol;
        if (!result->canonical_name[0] && item->ai_canonname) snprintf(result->canonical_name, sizeof(result->canonical_name), "%s", item->ai_canonname);
        ++result->count;
    }
    freeaddrinfo(list);
    return result->count ? 0 : LINUX_EAI_NONAME;
}
