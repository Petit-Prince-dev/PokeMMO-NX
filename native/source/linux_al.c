#include "linux_sdl.h"
#include "linux_audio.h"
#include <stdint.h>
#include <string.h>

// libopenal as the guest sees it: the entry points of OpenAL 1.1 and the two ALC extensions the client uses, all served by the
// software mixer of linux_audio.c (which the console's audio output pulls from).
typedef struct {
    const char *name;
    uintptr_t address;
} Entry;

static uintptr_t alLookup(const char *name);
static void *alcGetProcAddress(void *device, const char *name) {
    (void)device;
    return (void *)alLookup(name);
}
static void *alGetProcAddress(const char *name) { return (void *)alLookup(name); }

#define E(name, function) {name, (uintptr_t)(function)}
static const Entry table[] = {
    E("alcOpenDevice", linuxAlcOpenDevice),
    E("alcCloseDevice", linuxAlcCloseDevice),
    E("alcCreateContext", linuxAlcCreateContext),
    E("alcMakeContextCurrent", linuxAlcMakeContextCurrent),
    E("alcProcessContext", linuxAlcProcessContext),
    E("alcSuspendContext", linuxAlcSuspendContext),
    E("alcDestroyContext", linuxAlcDestroyContext),
    E("alcGetCurrentContext", linuxAlcGetCurrentContext),
    E("alcGetContextsDevice", linuxAlcGetContextsDevice),
    E("alcGetError", linuxAlcGetError),
    E("alcIsExtensionPresent", linuxAlcIsExtensionPresent),
    E("alcGetProcAddress", alcGetProcAddress),
    E("alcGetEnumValue", linuxAlcGetEnumValue),
    E("alcGetString", linuxAlcGetString),
    E("alcGetIntegerv", linuxAlcGetIntegerv),
    E("alcCaptureOpenDevice", linuxAlcCaptureOpenDevice),
    E("alcCaptureCloseDevice", linuxAlcCaptureCloseDevice),
    E("alcCaptureStart", linuxAlcCaptureStart),
    E("alcCaptureStop", linuxAlcCaptureStop),
    E("alcCaptureSamples", linuxAlcCaptureSamples),
    E("alcReopenDeviceSOFT", linuxAlcReopenDeviceSOFT),
    E("alcEventControlSOFT", linuxAlcEventControlSOFT),
    E("alcEventCallbackSOFT", linuxAlcEventCallbackSOFT),
    E("alEnable", linuxAlEnable),
    E("alDisable", linuxAlDisable),
    E("alIsEnabled", linuxAlIsEnabled),
    E("alGetString", linuxAlGetString),
    E("alGetBooleanv", linuxAlGetBooleanv),
    E("alGetIntegerv", linuxAlGetIntegerv),
    E("alGetFloatv", linuxAlGetFloatv),
    E("alGetDoublev", linuxAlGetDoublev),
    E("alGetBoolean", linuxAlGetBoolean),
    E("alGetInteger", linuxAlGetInteger),
    E("alGetFloat", linuxAlGetFloat),
    E("alGetDouble", linuxAlGetDouble),
    E("alGetError", linuxAlGetError),
    E("alIsExtensionPresent", linuxAlIsExtensionPresent),
    E("alGetProcAddress", alGetProcAddress),
    E("alGetEnumValue", linuxAlGetEnumValue),
    E("alDopplerFactor", linuxAlDopplerFactor),
    E("alDopplerVelocity", linuxAlDopplerVelocity),
    E("alSpeedOfSound", linuxAlSpeedOfSound),
    E("alDistanceModel", linuxAlDistanceModel),
    E("alListenerf", linuxAlListenerf),
    E("alListener3f", linuxAlListener3f),
    E("alListenerfv", linuxAlListenerfv),
    E("alListeneri", linuxAlListeneri),
    E("alListener3i", linuxAlListener3i),
    E("alListeneriv", linuxAlListeneriv),
    E("alGetListenerf", linuxAlGetListenerf),
    E("alGetListener3f", linuxAlGetListener3f),
    E("alGetListenerfv", linuxAlGetListenerfv),
    E("alGetListeneri", linuxAlGetListeneri),
    E("alGetListener3i", linuxAlGetListener3i),
    E("alGetListeneriv", linuxAlGetListeneriv),
    E("alGenBuffers", linuxAlGenBuffers),
    E("alDeleteBuffers", linuxAlDeleteBuffers),
    E("alIsBuffer", linuxAlIsBuffer),
    E("alBufferData", linuxAlBufferData),
    E("alBufferf", linuxAlBufferf),
    E("alBuffer3f", linuxAlBuffer3f),
    E("alBufferfv", linuxAlBufferfv),
    E("alBufferi", linuxAlBufferi),
    E("alBuffer3i", linuxAlBuffer3i),
    E("alBufferiv", linuxAlBufferiv),
    E("alGetBufferf", linuxAlGetBufferf),
    E("alGetBuffer3f", linuxAlGetBuffer3f),
    E("alGetBufferfv", linuxAlGetBufferfv),
    E("alGetBufferi", linuxAlGetBufferi),
    E("alGetBuffer3i", linuxAlGetBuffer3i),
    E("alGetBufferiv", linuxAlGetBufferiv),
    E("alGenSources", linuxAlGenSources),
    E("alDeleteSources", linuxAlDeleteSources),
    E("alIsSource", linuxAlIsSource),
    E("alSourcef", linuxAlSourcef),
    E("alSource3f", linuxAlSource3f),
    E("alSourcefv", linuxAlSourcefv),
    E("alSourcei", linuxAlSourcei),
    E("alSource3i", linuxAlSource3i),
    E("alSourceiv", linuxAlSourceiv),
    E("alGetSourcef", linuxAlGetSourcef),
    E("alGetSource3f", linuxAlGetSource3f),
    E("alGetSourcefv", linuxAlGetSourcefv),
    E("alGetSourcei", linuxAlGetSourcei),
    E("alGetSource3i", linuxAlGetSource3i),
    E("alGetSourceiv", linuxAlGetSourceiv),
    E("alSourcePlay", linuxAlSourcePlay),
    E("alSourceStop", linuxAlSourceStop),
    E("alSourceRewind", linuxAlSourceRewind),
    E("alSourcePause", linuxAlSourcePause),
    E("alSourcePlayv", linuxAlSourcePlayv),
    E("alSourceStopv", linuxAlSourceStopv),
    E("alSourceRewindv", linuxAlSourceRewindv),
    E("alSourcePausev", linuxAlSourcePausev),
    E("alSourceQueueBuffers", linuxAlSourceQueueBuffers),
    E("alSourceUnqueueBuffers", linuxAlSourceUnqueueBuffers),
};
static uintptr_t alLookup(const char *name) {
    for (size_t i = 0; name && i < sizeof(table) / sizeof(table[0]); ++i)
        if (!strcmp(table[i].name, name)) return table[i].address;
    return 0;
}
const LinuxVirtualLibrary linuxOpenAlLibrary = {"libopenal", alLookup, "al", false};
