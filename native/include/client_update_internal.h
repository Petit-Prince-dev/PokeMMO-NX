#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifndef UPDATE_ROOT
#define UPDATE_ROOT "sdmc:/switch/PokeMMO"
#endif
#define UPDATE_GAME UPDATE_ROOT "/isolate-root/game"
#define UPDATE_WORK UPDATE_ROOT "/.client-update"
#define UPDATE_ARCHIVE UPDATE_WORK "/client.zip"
#define UPDATE_MAX_FILES 2048u
#define UPDATE_NAME_BYTES 384u
#define UPDATE_MAX_ARCHIVE (1024ull * 1024ull * 1024ull)
#define UPDATE_MAX_EXPANDED (2ull * 1024ull * 1024ull * 1024ull)

typedef enum { UPDATE_CHECK, UPDATE_DOWNLOAD, UPDATE_EXTRACT, UPDATE_INSTALL, UPDATE_DONE, UPDATE_FAILED, UPDATE_BROKEN } UpdatePhase;
typedef struct {
    atomic_bool cancel, finished;
    atomic_uint phase, progress, resolver;
    atomic_ullong check_deadline;
    bool launch, network_ready;
} UpdateJob;

typedef struct UpdateHttp UpdateHttp;
typedef struct {
    uint64_t start, bytes, total;
    char etag[128];
} UpdateReply;
typedef struct {
    uint32_t revision;
    uint64_t bytes, expanded;
    char etag[128];
} UpdateRemote;
typedef struct {
    char name[UPDATE_NAME_BYTES];
    uint64_t bytes;
    uint32_t crc;
    uint8_t had_old, padding[3];
} UpdateEntry;
_Static_assert(sizeof(UpdateEntry) == 400, "update manifest layout");

uint64_t updateNow(void);
bool updateCancelled(const UpdateJob *job);
void updateProgress(UpdateJob *job, UpdatePhase phase, unsigned progress);
UpdateHttp *updateHttpOpen(UpdateJob *job);
void updateHttpClose(UpdateHttp *http);
bool updateHttpRange(UpdateHttp *http, const char *range, const char *etag, void *data, size_t capacity, UpdateReply *reply);
bool updateHttpDownload(UpdateHttp *http, const UpdateRemote *remote);
bool updateRemoteRevision(UpdateHttp *http, UpdateRemote *remote);
uint32_t updateInstalledRevision(const char *path);
bool updatePrepare(UpdateJob *job, uint32_t revision, UpdateEntry **entries, unsigned *count);
bool updateInstall(UpdateJob *job, const UpdateEntry *entries, unsigned count);
bool updateRecover(UpdateJob *job);
bool updateClean(void);
bool updateMakeParents(const char *path);
bool updateCommit(void);
bool updateManagedName(const char *name);

bool updateScreenOpen(void);
void updateScreenDraw(unsigned progress, const char *message, const char *detail);
void updateScreenClose(void);
