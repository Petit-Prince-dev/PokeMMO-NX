#include "client_update_internal.h"
#include "diagnostics.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>

#define HOST "dl.pokemmo.com"
#define RESOURCE "/download/PokeMMO-Client.zip"
#define IO_IDLE_MS 15000u

struct UpdateHttp {
    UpdateJob *job;
    SslContext context;
    SslConnection connection;
    bool ssl_ready, context_ready, connected, close_after;
    int socket, ssl_socket;
    uint64_t expires;
    unsigned char buffer[16384];
    size_t position, available;
};
static bool stopped(UpdateHttp *http) { return updateCancelled(http->job) || updateNow() >= http->expires; }
static void advanced(UpdateHttp *http) {
    http->expires = atomic_load(&http->job->phase) == UPDATE_CHECK ? atomic_load(&http->job->check_deadline) : updateNow() + IO_IDLE_MS;
}
static void disconnect(UpdateHttp *http) {
    if (http->connected) {
        if (http->ssl_socket >= 0) close(http->ssl_socket);
        sslConnectionClose(&http->connection);
    }
    if (http->socket >= 0) close(http->socket);
    http->socket = http->ssl_socket = -1;
    http->connected = false;
    http->position = http->available = 0;
}
void updateHttpClose(UpdateHttp *http) {
    if (!http) return;
    disconnect(http);
    if (http->context_ready) sslContextClose(&http->context);
    if (http->ssl_ready) sslExit();
    free(http);
}
UpdateHttp *updateHttpOpen(UpdateJob *job) {
    u32 ip = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || !ip) return NULL;
    UpdateHttp *http = calloc(1, sizeof(*http));
    if (!http) return NULL;
    http->job = job;
    http->socket = http->ssl_socket = -1;
    if (R_FAILED(sslInitialize(1))) goto failed;
    http->ssl_ready = true;
    u32 versions = SslVersion_TlsV12;
    if (hosversionAtLeast(11, 0, 0)) versions |= SslVersion_TlsV13;
    if (R_FAILED(sslCreateContext(&http->context, versions))) goto failed;
    http->context_ready = true;
    return http;
failed:
    updateHttpClose(http);
    return NULL;
}
static bool connectServer(UpdateHttp *http) {
    advanced(http);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *addresses = NULL;
    resolverSetEnableServiceDiscovery(false);
    atomic_store(&http->job->resolver, resolverGetCancelHandle());
    if (stopped(http)) {
        atomic_store(&http->job->resolver, 0);
        return false;
    }
    int resolved = getaddrinfo(HOST, "443", &hints, &addresses);
    atomic_store(&http->job->resolver, 0);
    if (resolved || stopped(http)) {
        if (addresses) freeaddrinfo(addresses);
        return false;
    }
    for (struct addrinfo *address = addresses; address && !stopped(http); address = address->ai_next) {
        int descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (descriptor < 0) continue;
        int flags = fcntl(descriptor, F_GETFL, 0);
        if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK)) {
            close(descriptor);
            continue;
        }
        int result = connect(descriptor, address->ai_addr, address->ai_addrlen);
        if (result && errno != EINPROGRESS && errno != EWOULDBLOCK) {
            close(descriptor);
            continue;
        }
        while (result && !stopped(http)) {
            struct pollfd waiting = {.fd = descriptor, .events = POLLOUT};
            int ready = poll(&waiting, 1, 50);
            if (ready < 0) break;
            if (!ready) continue;
            int error = 0;
            socklen_t bytes = sizeof(error);
            result = getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &bytes) || error;
            break;
        }
        if (!result && !stopped(http)) {
            http->socket = descriptor;
            break;
        }
        close(descriptor);
    }
    freeaddrinfo(addresses);
    if (http->socket < 0 || stopped(http)) return false;
    if (R_FAILED(sslContextCreateConnection(&http->context, &http->connection))) goto failed;
    http->connected = true;
    if (R_FAILED(sslConnectionSetHostName(&http->connection, HOST, sizeof(HOST) - 1)) ||
        R_FAILED(sslConnectionSetVerifyOption(&http->connection, SslVerifyOption_PeerCa | SslVerifyOption_HostName | SslVerifyOption_DateCheck)) ||
        R_FAILED(sslConnectionSetOption(&http->connection, SslOptionType_DoNotCloseSocket, true)))
        goto failed;
    errno = 0;
    http->ssl_socket = socketSslConnectionSetSocketDescriptor(&http->connection, http->socket);
    if (http->ssl_socket < 0 && errno != ENOENT) goto failed;
    if (R_FAILED(sslConnectionSetIoMode(&http->connection, SslIoMode_NonBlocking))) goto failed;
    while (!stopped(http)) {
        Result rc = sslConnectionDoHandshake(&http->connection, NULL, NULL, NULL, 0);
        if (R_SUCCEEDED(rc)) {
            advanced(http);
            return true;
        }
        // Non-blocking TLS can need several reads/writes. The absolute check deadline also bounds terminal TLS failures.
        svcSleepThread(5000000);
    }
failed:
    disconnect(http);
    return false;
}
static bool receive(UpdateHttp *http) {
    http->position = http->available = 0;
    while (!stopped(http)) {
        u32 got = 0;
        Result rc = sslConnectionRead(&http->connection, http->buffer, sizeof(http->buffer), &got);
        if (R_SUCCEEDED(rc)) {
            if (!got) return false;
            http->available = got;
            advanced(http);
            return true;
        }
        svcSleepThread(5000000);
    }
    return false;
}
static bool readBytes(UpdateHttp *http, void *output, size_t bytes) {
    unsigned char *target = output;
    while (bytes && !stopped(http)) {
        if (http->position == http->available && !receive(http)) return false;
        size_t take = http->available - http->position;
        if (take > bytes) take = bytes;
        memcpy(target, http->buffer + http->position, take);
        target += take;
        http->position += take;
        bytes -= take;
    }
    return bytes == 0;
}
static bool readLine(UpdateHttp *http, char *line, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        char value;
        if (!readBytes(http, &value, 1) || value == 0) return false;
        if (value == '\n') {
            if (!used || line[used - 1] != '\r') return false;
            line[used - 1] = 0;
            return true;
        }
        line[used++] = value;
    }
    return false;
}
static bool number(const char *text, uint64_t *value) {
    if (!*text) return false;
    uint64_t result = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || result > (UINT64_MAX - (unsigned)(*text - '0')) / 10) return false;
        result = result * 10 + (unsigned)(*text - '0');
    }
    *value = result;
    return true;
}
static bool request(UpdateHttp *http, const char *range, const char *etag, UpdateReply *reply) {
    memset(reply, 0, sizeof(*reply));
    advanced(http);
    if (!http->connected && !connectServer(http)) return false;
    char message[768];
    int length =
        snprintf(message, sizeof(message),
                 "GET " RESOURCE " HTTP/1.1\r\nHost: " HOST "\r\nUser-Agent: PokeMMO-NX/" APP_VERSION
                 "\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n%s%s%s%s%s%s\r\n",
                 range ? "Range: bytes=" : "", range ? range : "", range ? "\r\n" : "", etag ? "If-Match: " : "", etag ? etag : "", etag ? "\r\n" : "");
    if (length <= 0 || length >= (int)sizeof(message)) return false;
    size_t sent = 0;
    while (sent < (size_t)length && !stopped(http)) {
        u32 count = 0;
        Result rc = sslConnectionWrite(&http->connection, message + sent, length - sent, &count);
        if (R_SUCCEEDED(rc)) {
            if (!count) return false;
            sent += count;
            advanced(http);
        } else
            svcSleepThread(5000000);
    }
    if (sent != (size_t)length) return false;
    char line[1024];
    unsigned major, minor, status;
    if (!readLine(http, line, sizeof(line)) || sscanf(line, "HTTP/%u.%u %u", &major, &minor, &status) != 3 || major != 1 || minor > 1 ||
        status != (range ? 206u : 200u))
        return false;
    bool length_seen = false, range_seen = false;
    uint64_t range_bytes = 0;
    unsigned header_bytes = 0;
    http->close_after = minor == 0;
    while (readLine(http, line, sizeof(line))) {
        header_bytes += strlen(line) + 2;
        if (header_bytes > 16384) return false;
        if (!*line) {
            if (!length_seen || !reply->bytes || reply->bytes > UPDATE_MAX_ARCHIVE || (range && !range_seen) || (range && range_bytes != reply->bytes) ||
                reply->etag[0] != '"' || reply->etag[strlen(reply->etag) - 1] != '"')
                return false;
            if (!range) reply->total = reply->bytes;
            return !etag || !strcmp(reply->etag, etag);
        }
        char *colon = strchr(line, ':');
        if (!colon) return false;
        *colon++ = 0;
        while (*colon == ' ' || *colon == '\t') ++colon;
        char *end = colon + strlen(colon);
        while (end > colon && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!strcasecmp(line, "Content-Length")) {
            if (length_seen || !number(colon, &reply->bytes)) return false;
            length_seen = true;
        } else if (!strcasecmp(line, "Content-Range")) {
            unsigned long long first, last, total;
            char extra;
            if (range_seen || sscanf(colon, "bytes %llu-%llu/%llu%c", &first, &last, &total, &extra) != 3 || first > last || last >= total ||
                total > UPDATE_MAX_ARCHIVE)
                return false;
            reply->start = first;
            reply->total = total;
            range_bytes = last - first + 1;
            if (range_bytes > UPDATE_MAX_ARCHIVE) return false;
            range_seen = true;
        } else if (!strcasecmp(line, "ETag")) {
            if (*reply->etag || strlen(colon) >= sizeof(reply->etag)) return false;
            strcpy(reply->etag, colon);
        } else if (!strcasecmp(line, "Transfer-Encoding") && strcasecmp(colon, "identity"))
            return false;
        else if (!strcasecmp(line, "Content-Encoding") && strcasecmp(colon, "identity"))
            return false;
        else if (!strcasecmp(line, "Connection") && !strcasecmp(colon, "close"))
            http->close_after = true;
    }
    return false;
}
bool updateHttpRange(UpdateHttp *http, const char *range, const char *etag, void *data, size_t capacity, UpdateReply *reply) {
    bool ok = request(http, range, etag, reply) && reply->bytes <= capacity && reply->start <= reply->total && reply->bytes <= reply->total - reply->start &&
              readBytes(http, data, reply->bytes);
    if (!ok || http->close_after) disconnect(http);
    return ok;
}
bool updateHttpDownload(UpdateHttp *http, const UpdateRemote *remote) {
    UpdateReply reply;
    if (!request(http, NULL, remote->etag, &reply) || reply.bytes != remote->bytes) return false;
    FILE *file = fopen(UPDATE_ARCHIVE, "wb");
    unsigned char *buffer = file ? malloc(65536) : NULL;
    bool ok = buffer != NULL;
    uint64_t done = 0;
    while (ok && done < reply.bytes && !updateCancelled(http->job)) {
        size_t take = reply.bytes - done < 65536 ? (size_t)(reply.bytes - done) : 65536;
        ok = readBytes(http, buffer, take) && fwrite(buffer, 1, take, file) == take;
        if (ok) done += take;
        updateProgress(http->job, UPDATE_DOWNLOAD, (unsigned)(done * 700 / reply.bytes));
    }
    if (file) {
        if (fflush(file) || fsync(fileno(file))) ok = false;
        if (fclose(file)) ok = false;
    }
    free(buffer);
    disconnect(http);
    return ok && done == remote->bytes && !updateCancelled(http->job);
}
