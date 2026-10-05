#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// The game's OpenAL: a software mixer that implements OpenAL 1.1 (buffers, sources, queues, listener, distance models) plus the two
// extensions the client uses (ALC_SOFT_reopen_device, ALC_SOFT_system_events). linux_al.c exposes it to the guest as "libopenal";
// the console's audio output (linux_audio_out.c) pulls 48 kHz stereo blocks from linuxAudioMix. One device and one context exist at a
// time; names are small integers.
//
// Types follow the C ABI of OpenAL: ALenum/ALint/ALsizei/ALuint are 32-bit, ALboolean/ALCboolean are one byte.

#define LINUX_AUDIO_RATE 48000u

typedef struct {
    bool (*start)(void);  // begins calling linuxAudioMix (from its own thread) at the real-time rate
    void (*stop)(void);   // returns once no call to linuxAudioMix is running or will start
    void (*yield)(void);  // called while waiting for the internal lock: lets a lower-priority holder run
} LinuxAudioOutput;
void linuxAudioSetOutput(const LinuxAudioOutput *output);
// Console only (linux_audio_out.c): registers the audout output with the mixer; sets the log of both.
void linuxAudioOutAttach(void);

// Mixes `frames` stereo frames (interleaved int16) of everything that plays. Silence when nothing does.
void linuxAudioMix(int16_t *interleaved, unsigned frames);
// Stops the output, forgets every device, context, source and buffer, writes the summary to the log.
void linuxAudioReset(void);

// ---- ALC ----------------------------------------------------------------------------------------------------------------------
void *linuxAlcOpenDevice(const char *name);
unsigned char linuxAlcCloseDevice(void *device);
void *linuxAlcCreateContext(void *device, const int *attributes);
unsigned char linuxAlcMakeContextCurrent(void *context);
void linuxAlcProcessContext(void *context);
void linuxAlcSuspendContext(void *context);
void linuxAlcDestroyContext(void *context);
void *linuxAlcGetCurrentContext(void);
void *linuxAlcGetContextsDevice(void *context);
int linuxAlcGetError(void *device);
unsigned char linuxAlcIsExtensionPresent(void *device, const char *name);
int linuxAlcGetEnumValue(void *device, const char *name);
const char *linuxAlcGetString(void *device, int parameter);
void linuxAlcGetIntegerv(void *device, int parameter, int size, int *values);
void *linuxAlcCaptureOpenDevice(const char *name, unsigned frequency, int format, int size);
unsigned char linuxAlcCaptureCloseDevice(void *device);
void linuxAlcCaptureStart(void *device);
void linuxAlcCaptureStop(void *device);
void linuxAlcCaptureSamples(void *device, void *buffer, int samples);
unsigned char linuxAlcReopenDeviceSOFT(void *device, const char *name, const int *attributes);
unsigned char linuxAlcEventControlSOFT(int count, const int *events, unsigned char enable);
void linuxAlcEventCallbackSOFT(void *callback, void *user);

// ---- AL -----------------------------------------------------------------------------------------------------------------------
void linuxAlEnable(int capability);
void linuxAlDisable(int capability);
unsigned char linuxAlIsEnabled(int capability);
const char *linuxAlGetString(int parameter);
unsigned char linuxAlGetBoolean(int parameter);
int linuxAlGetInteger(int parameter);
float linuxAlGetFloat(int parameter);
double linuxAlGetDouble(int parameter);
void linuxAlGetBooleanv(int parameter, unsigned char *values);
void linuxAlGetIntegerv(int parameter, int *values);
void linuxAlGetFloatv(int parameter, float *values);
void linuxAlGetDoublev(int parameter, double *values);
int linuxAlGetError(void);
unsigned char linuxAlIsExtensionPresent(const char *name);
int linuxAlGetEnumValue(const char *name);
void linuxAlDopplerFactor(float value);
void linuxAlDopplerVelocity(float value);
void linuxAlSpeedOfSound(float value);
void linuxAlDistanceModel(int model);

void linuxAlListenerf(int parameter, float value);
void linuxAlListener3f(int parameter, float a, float b, float c);
void linuxAlListenerfv(int parameter, const float *values);
void linuxAlListeneri(int parameter, int value);
void linuxAlListener3i(int parameter, int a, int b, int c);
void linuxAlListeneriv(int parameter, const int *values);
void linuxAlGetListenerf(int parameter, float *value);
void linuxAlGetListener3f(int parameter, float *a, float *b, float *c);
void linuxAlGetListenerfv(int parameter, float *values);
void linuxAlGetListeneri(int parameter, int *value);
void linuxAlGetListener3i(int parameter, int *a, int *b, int *c);
void linuxAlGetListeneriv(int parameter, int *values);

void linuxAlGenBuffers(int count, unsigned *names);
void linuxAlDeleteBuffers(int count, const unsigned *names);
unsigned char linuxAlIsBuffer(unsigned name);
void linuxAlBufferData(unsigned name, int format, const void *data, int size, int frequency);
void linuxAlBufferf(unsigned name, int parameter, float value);
void linuxAlBuffer3f(unsigned name, int parameter, float a, float b, float c);
void linuxAlBufferfv(unsigned name, int parameter, const float *values);
void linuxAlBufferi(unsigned name, int parameter, int value);
void linuxAlBuffer3i(unsigned name, int parameter, int a, int b, int c);
void linuxAlBufferiv(unsigned name, int parameter, const int *values);
void linuxAlGetBufferf(unsigned name, int parameter, float *value);
void linuxAlGetBuffer3f(unsigned name, int parameter, float *a, float *b, float *c);
void linuxAlGetBufferfv(unsigned name, int parameter, float *values);
void linuxAlGetBufferi(unsigned name, int parameter, int *value);
void linuxAlGetBuffer3i(unsigned name, int parameter, int *a, int *b, int *c);
void linuxAlGetBufferiv(unsigned name, int parameter, int *values);

void linuxAlGenSources(int count, unsigned *names);
void linuxAlDeleteSources(int count, const unsigned *names);
unsigned char linuxAlIsSource(unsigned name);
void linuxAlSourcef(unsigned name, int parameter, float value);
void linuxAlSource3f(unsigned name, int parameter, float a, float b, float c);
void linuxAlSourcefv(unsigned name, int parameter, const float *values);
void linuxAlSourcei(unsigned name, int parameter, int value);
void linuxAlSource3i(unsigned name, int parameter, int a, int b, int c);
void linuxAlSourceiv(unsigned name, int parameter, const int *values);
void linuxAlGetSourcef(unsigned name, int parameter, float *value);
void linuxAlGetSource3f(unsigned name, int parameter, float *a, float *b, float *c);
void linuxAlGetSourcefv(unsigned name, int parameter, float *values);
void linuxAlGetSourcei(unsigned name, int parameter, int *value);
void linuxAlGetSource3i(unsigned name, int parameter, int *a, int *b, int *c);
void linuxAlGetSourceiv(unsigned name, int parameter, int *values);
void linuxAlSourcePlay(unsigned name);
void linuxAlSourceStop(unsigned name);
void linuxAlSourceRewind(unsigned name);
void linuxAlSourcePause(unsigned name);
void linuxAlSourcePlayv(int count, const unsigned *names);
void linuxAlSourceStopv(int count, const unsigned *names);
void linuxAlSourceRewindv(int count, const unsigned *names);
void linuxAlSourcePausev(int count, const unsigned *names);
void linuxAlSourceQueueBuffers(unsigned name, int count, const unsigned *buffers);
void linuxAlSourceUnqueueBuffers(unsigned name, int count, unsigned *buffers);
