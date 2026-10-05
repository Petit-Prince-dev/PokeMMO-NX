#include "linux_audio.h"
#include "diagnostics.h"
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// ---- OpenAL constants ---------------------------------------------------------------------------------------------------------
enum {
    AL_SOURCE_RELATIVE = 0x202,
    AL_CONE_INNER_ANGLE = 0x1001,
    AL_CONE_OUTER_ANGLE = 0x1002,
    AL_PITCH = 0x1003,
    AL_POSITION = 0x1004,
    AL_DIRECTION = 0x1005,
    AL_VELOCITY = 0x1006,
    AL_LOOPING = 0x1007,
    AL_BUFFER = 0x1009,
    AL_GAIN = 0x100A,
    AL_MIN_GAIN = 0x100D,
    AL_MAX_GAIN = 0x100E,
    AL_ORIENTATION = 0x100F,
    AL_SOURCE_STATE = 0x1010,
    AL_INITIAL = 0x1011,
    AL_PLAYING = 0x1012,
    AL_PAUSED = 0x1013,
    AL_STOPPED = 0x1014,
    AL_BUFFERS_QUEUED = 0x1015,
    AL_BUFFERS_PROCESSED = 0x1016,
    AL_REFERENCE_DISTANCE = 0x1020,
    AL_ROLLOFF_FACTOR = 0x1021,
    AL_CONE_OUTER_GAIN = 0x1022,
    AL_MAX_DISTANCE = 0x1023,
    AL_SEC_OFFSET = 0x1024,
    AL_SAMPLE_OFFSET = 0x1025,
    AL_BYTE_OFFSET = 0x1026,
    AL_SOURCE_TYPE = 0x1027,
    AL_STATIC = 0x1028,
    AL_STREAMING = 0x1029,
    AL_UNDETERMINED = 0x1030,
    AL_DIRECT_CHANNELS_SOFT = 0x1033,
    AL_STOP_SOURCES_ON_DISCONNECT_SOFT = 0x19AB,
    AL_FORMAT_MONO8 = 0x1100,
    AL_FORMAT_MONO16 = 0x1101,
    AL_FORMAT_STEREO8 = 0x1102,
    AL_FORMAT_STEREO16 = 0x1103,
    AL_FORMAT_MONO_FLOAT32 = 0x10010,
    AL_FORMAT_STEREO_FLOAT32 = 0x10011,
    AL_FREQUENCY = 0x2001,
    AL_BITS = 0x2002,
    AL_CHANNELS = 0x2003,
    AL_SIZE = 0x2004,
    AL_NO_ERROR = 0,
    AL_INVALID_NAME = 0xA001,
    AL_INVALID_ENUM = 0xA002,
    AL_INVALID_VALUE = 0xA003,
    AL_INVALID_OPERATION = 0xA004,
    AL_OUT_OF_MEMORY = 0xA005,
    AL_VENDOR = 0xB001,
    AL_VERSION = 0xB002,
    AL_RENDERER = 0xB003,
    AL_EXTENSIONS = 0xB004,
    AL_DOPPLER_FACTOR = 0xC000,
    AL_DOPPLER_VELOCITY = 0xC001,
    AL_SPEED_OF_SOUND = 0xC003,
    AL_DISTANCE_MODEL = 0xD000,
    AL_INVERSE_DISTANCE = 0xD001,
    AL_INVERSE_DISTANCE_CLAMPED = 0xD002,
    AL_LINEAR_DISTANCE = 0xD003,
    AL_LINEAR_DISTANCE_CLAMPED = 0xD004,
    AL_EXPONENT_DISTANCE = 0xD005,
    AL_EXPONENT_DISTANCE_CLAMPED = 0xD006,
    ALC_MAJOR_VERSION = 0x1000,
    ALC_MINOR_VERSION = 0x1001,
    ALC_ATTRIBUTES_SIZE = 0x1002,
    ALC_ALL_ATTRIBUTES = 0x1003,
    ALC_DEFAULT_DEVICE_SPECIFIER = 0x1004,
    ALC_DEVICE_SPECIFIER = 0x1005,
    ALC_EXTENSIONS = 0x1006,
    ALC_FREQUENCY = 0x1007,
    ALC_REFRESH = 0x1008,
    ALC_SYNC = 0x1009,
    ALC_MONO_SOURCES = 0x1010,
    ALC_STEREO_SOURCES = 0x1011,
    ALC_DEFAULT_ALL_DEVICES_SPECIFIER = 0x1012,
    ALC_ALL_DEVICES_SPECIFIER = 0x1013,
    ALC_CAPTURE_DEVICE_SPECIFIER = 0x310,
    ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER = 0x311,
    ALC_CAPTURE_SAMPLES = 0x312,
    ALC_NO_ERROR = 0,
    ALC_INVALID_DEVICE = 0xA001,
    ALC_INVALID_CONTEXT = 0xA002,
    ALC_INVALID_ENUM = 0xA003,
    ALC_INVALID_VALUE = 0xA004
};

#define MAX_BUFFERS 4096u
#define MAX_SOURCES 256u
#define MAX_QUEUE 128u
#define MAX_BLOCK 4096u
#define LOG_LINES 160u

typedef struct {
    int16_t *data;           // interleaved, every format converted to 16 bits
    uint32_t frames, bytes;  // bytes: what the game uploaded, for AL_SIZE
    int32_t frequency;
    uint8_t channels, bits;  // bits: 8, 16 or 32 as uploaded
    bool used;
    uint32_t references;  // queue entries that point here
} Buffer;
typedef struct {
    bool used, relative, looping, offset_pending;
    int state, type, direct_channels;  // direct_channels: AL_DIRECT_CHANNELS_SOFT, kept but without effect (stereo is never spatialized)
    float gain, pitch, min_gain, max_gain, reference_distance, rolloff, max_distance, cone_inner, cone_outer, cone_outer_gain;
    float position[3], velocity[3], direction[3];
    uint32_t queue[MAX_QUEUE];
    uint32_t count, current, processed;  // current: queue index being played, processed: entries completely played
    uint32_t sample, fraction;           // position inside the current entry: whole frames and 1/2^32 of a frame
} Source;

static Buffer buffers[MAX_BUFFERS + 1];
static Source sources[MAX_SOURCES + 1];
static struct {
    float gain, position[3], velocity[3], orientation[6];
} listener;
static int distance_model, last_error, last_alc_error;
static float doppler_factor, doppler_velocity, speed_of_sound;
static unsigned devices_open, contexts_open;
static bool context_current, initialized;
static char device_object[16], context_object[16];
static void *event_callback, *event_user;
static LinuxAudioOutput output;
static bool output_running;
typedef struct {
    unsigned devices, contexts, buffers_created, sources_created, buffer_data_calls, plays, queued_buffers, unqueued_buffers, errors, max_playing;
    uint64_t buffer_bytes, mixed_frames, mix_calls;
    int peak;
} LinuxAudioStats;
static LinuxAudioStats stats;  // counted for the diagnostics summary only
static unsigned log_lines;     // lines traced so far (under the audio lock)
static atomic_flag lock_flag = ATOMIC_FLAG_INIT;

static void lockAudio(void) {
    while (atomic_flag_test_and_set_explicit(&lock_flag, memory_order_acquire)) {
        if (output.yield) output.yield();
    }
}
static void unlockAudio(void) { atomic_flag_clear_explicit(&lock_flag, memory_order_release); }

void linuxAudioSetOutput(const LinuxAudioOutput *hooks) {
    lockAudio();
    if (hooks)
        output = *hooks;
    else
        memset(&output, 0, sizeof(output));
    unlockAudio();
}
static void trace(const char *format, ...) {
    if (log_lines >= LOG_LINES) return;
    ++log_lines;
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}

static void ensureInit(void) {  // lock held
    if (initialized) return;
    initialized = true;
    listener.gain = 1;
    listener.orientation[2] = -1;
    listener.orientation[4] = 1;
    distance_model = AL_INVERSE_DISTANCE_CLAMPED;
    doppler_factor = 1;
    doppler_velocity = 1;
    speed_of_sound = 343.3f;
}
static int last_parameter;                           // the parameter of the call being served, for the log
static void setError(int code, const char *where) {  // lock held
    ++stats.errors;
    if (last_error == AL_NO_ERROR) last_error = code;
    trace("audio.error code=0x%x where=%s parameter=0x%x", code, where, (unsigned)last_parameter);
}
static void setAlcError(int code) {
    if (last_alc_error == ALC_NO_ERROR) last_alc_error = code;
}

// ---- buffers ------------------------------------------------------------------------------------------------------------------
static Buffer *bufferByName(unsigned name) { return name >= 1 && name <= MAX_BUFFERS && buffers[name].used ? &buffers[name] : NULL; }
static Source *sourceByName(unsigned name) { return name >= 1 && name <= MAX_SOURCES && sources[name].used ? &sources[name] : NULL; }
static void releaseBuffer(Buffer *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static void initSource(Source *s) {
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->state = AL_INITIAL;
    s->type = AL_UNDETERMINED;
    s->gain = 1;
    s->pitch = 1;
    s->max_gain = 1;
    s->reference_distance = 1;
    s->rolloff = 1;
    s->max_distance = 3.4028235e38f;
    s->cone_inner = s->cone_outer = 360;
    s->direction[0] = s->direction[1] = s->direction[2] = 0;
}
static void clearQueue(Source *s) {
    for (uint32_t i = 0; i < s->count; ++i) {
        Buffer *b = bufferByName(s->queue[i]);
        if (b && b->references) --b->references;
    }
    s->count = s->current = s->processed = 0;
    s->sample = s->fraction = 0;
    s->offset_pending = false;
}
static void rewindSource(Source *s) {
    s->state = AL_INITIAL;
    s->current = s->processed = 0;
    s->sample = s->fraction = 0;
    s->offset_pending = false;
}

void linuxAlGenBuffers(int count, unsigned *names) {
    lockAudio();
    ensureInit();
    if (count < 0) {
        setError(AL_INVALID_VALUE, "alGenBuffers");
        unlockAudio();
        return;
    }
    int made = 0;
    for (unsigned n = 1; n <= MAX_BUFFERS && made < count && names; ++n)
        if (!buffers[n].used) {
            buffers[n].used = true;
            names[made++] = n;
        }
    if (made < count && names) {
        for (int i = 0; i < made; ++i) buffers[names[i]].used = false;
        setError(AL_OUT_OF_MEMORY, "alGenBuffers");
    } else
        stats.buffers_created += (unsigned)made;
    unlockAudio();
}
void linuxAlDeleteBuffers(int count, const unsigned *names) {
    lockAudio();
    ensureInit();
    if (count < 0) {
        setError(AL_INVALID_VALUE, "alDeleteBuffers");
        unlockAudio();
        return;
    }
    bool ok = true;
    for (int i = 0; i < count && names; ++i) {
        if (!names[i]) continue;
        Buffer *b = bufferByName(names[i]);
        if (!b) {
            setError(AL_INVALID_NAME, "alDeleteBuffers");
            ok = false;
            break;
        }
        if (b->references) {
            setError(AL_INVALID_OPERATION, "alDeleteBuffers");
            ok = false;
            break;
        }
    }
    if (ok)
        for (int i = 0; i < count && names; ++i) {
            Buffer *b = bufferByName(names[i]);
            if (b) releaseBuffer(b);
        }
    unlockAudio();
}
unsigned char linuxAlIsBuffer(unsigned name) {
    lockAudio();
    unsigned char value = name == 0 || bufferByName(name) ? 1 : 0;
    unlockAudio();
    return value;
}

static int16_t clampSample(float value) { return (int16_t)(value > 1 ? 32767 : (value < -1 ? -32767 : value * 32767.0f)); }
void linuxAlBufferData(unsigned name, int format, const void *data, int size, int frequency) {
    lockAudio();
    ensureInit();
    Buffer *b = bufferByName(name);
    unsigned channels = 0, bits = 0;
    switch (format) {
        case AL_FORMAT_MONO8:
            channels = 1;
            bits = 8;
            break;
        case AL_FORMAT_MONO16:
            channels = 1;
            bits = 16;
            break;
        case AL_FORMAT_STEREO8:
            channels = 2;
            bits = 8;
            break;
        case AL_FORMAT_STEREO16:
            channels = 2;
            bits = 16;
            break;
        case AL_FORMAT_MONO_FLOAT32:
            channels = 1;
            bits = 32;
            break;
        case AL_FORMAT_STEREO_FLOAT32:
            channels = 2;
            bits = 32;
            break;
        default: break;
    }
    if (!b)
        setError(AL_INVALID_NAME, "alBufferData");
    else if (b->references)
        setError(AL_INVALID_OPERATION, "alBufferData");
    else if (!channels)
        setError(AL_INVALID_ENUM, "alBufferData");
    else if (size < 0 || frequency <= 0 || (unsigned)size % (channels * bits / 8))
        setError(AL_INVALID_VALUE, "alBufferData");
    else {
        uint32_t frames = (uint32_t)size / (channels * bits / 8);
        size_t samples = (size_t)frames * channels;
        int16_t *converted = samples ? (int16_t *)malloc(samples * sizeof(int16_t)) : NULL;
        if (samples && !converted)
            setError(AL_OUT_OF_MEMORY, "alBufferData");
        else {
            for (size_t i = 0; i < samples; ++i) {
                if (!data)
                    converted[i] = 0;
                else if (bits == 8)
                    converted[i] = (int16_t)(((int)((const uint8_t *)data)[i] - 128) << 8);
                else if (bits == 16) {
                    int16_t v;
                    memcpy(&v, (const uint8_t *)data + i * 2, 2);
                    converted[i] = v;
                } else {
                    float v;
                    memcpy(&v, (const uint8_t *)data + i * 4, 4);
                    converted[i] = clampSample(v);
                }
            }
            free(b->data);
            b->data = converted;
            b->frames = frames;
            b->bytes = (uint32_t)size;
            b->frequency = frequency;
            b->channels = (uint8_t)channels;
            b->bits = (uint8_t)bits;
            ++stats.buffer_data_calls;
            stats.buffer_bytes += (uint64_t)size;
            if (stats.buffer_data_calls <= 24)
                trace("audio.buffer.data name=%u format=0x%x channels=%u bits=%u frames=%u frequency=%d", name, (unsigned)format, channels, bits, frames,
                      frequency);
        }
    }
    unlockAudio();
}
void linuxAlBufferf(unsigned name, int parameter, float value) {
    (void)parameter;
    (void)value;
    lockAudio();
    ensureInit();
    setError(bufferByName(name) ? AL_INVALID_ENUM : AL_INVALID_NAME, "alBufferf");
    unlockAudio();
}
void linuxAlBuffer3f(unsigned name, int parameter, float a, float b, float c) {
    (void)a;
    (void)b;
    (void)c;
    linuxAlBufferf(name, parameter, 0);
}
void linuxAlBufferfv(unsigned name, int parameter, const float *values) {
    (void)values;
    linuxAlBufferf(name, parameter, 0);
}
void linuxAlBufferi(unsigned name, int parameter, int value) {
    (void)value;
    linuxAlBufferf(name, parameter, 0);
}
void linuxAlBuffer3i(unsigned name, int parameter, int a, int b, int c) {
    (void)a;
    (void)b;
    (void)c;
    linuxAlBufferf(name, parameter, 0);
}
void linuxAlBufferiv(unsigned name, int parameter, const int *values) {
    (void)values;
    linuxAlBufferf(name, parameter, 0);
}
static bool bufferValue(unsigned name, int parameter, double *value, const char *where) {
    lockAudio();
    ensureInit();
    Buffer *b = bufferByName(name);
    bool ok = false;
    if (!b)
        setError(AL_INVALID_NAME, where);
    else
        switch (parameter) {
            case AL_FREQUENCY:
                *value = b->frequency;
                ok = true;
                break;
            case AL_BITS:
                *value = b->bits;
                ok = true;
                break;
            case AL_CHANNELS:
                *value = b->channels;
                ok = true;
                break;
            case AL_SIZE:
                *value = b->bytes;
                ok = true;
                break;
            default: setError(AL_INVALID_ENUM, where); break;
        }
    unlockAudio();
    return ok;
}
void linuxAlGetBufferf(unsigned name, int parameter, float *value) {
    double v;
    if (!value) {
        lockAudio();
        setError(AL_INVALID_VALUE, "alGetBufferf");
        unlockAudio();
    } else if (bufferValue(name, parameter, &v, "alGetBufferf"))
        *value = (float)v;
}
void linuxAlGetBuffer3f(unsigned name, int parameter, float *a, float *b, float *c) {
    (void)a;
    (void)b;
    (void)c;
    double v;
    bufferValue(name, parameter, &v, "alGetBuffer3f");
    lockAudio();
    setError(AL_INVALID_ENUM, "alGetBuffer3f");
    unlockAudio();
}
void linuxAlGetBufferfv(unsigned name, int parameter, float *values) { linuxAlGetBufferf(name, parameter, values); }
void linuxAlGetBufferi(unsigned name, int parameter, int *value) {
    double v;
    if (!value) {
        lockAudio();
        setError(AL_INVALID_VALUE, "alGetBufferi");
        unlockAudio();
    } else if (bufferValue(name, parameter, &v, "alGetBufferi"))
        *value = (int)v;
}
void linuxAlGetBuffer3i(unsigned name, int parameter, int *a, int *b, int *c) {
    (void)a;
    (void)b;
    (void)c;
    double v;
    bufferValue(name, parameter, &v, "alGetBuffer3i");
    lockAudio();
    setError(AL_INVALID_ENUM, "alGetBuffer3i");
    unlockAudio();
}
void linuxAlGetBufferiv(unsigned name, int parameter, int *values) { linuxAlGetBufferi(name, parameter, values); }

// ---- sources ------------------------------------------------------------------------------------------------------------------
void linuxAlGenSources(int count, unsigned *names) {
    lockAudio();
    ensureInit();
    if (count < 0) {
        setError(AL_INVALID_VALUE, "alGenSources");
        unlockAudio();
        return;
    }
    int made = 0;
    for (unsigned n = 1; n <= MAX_SOURCES && made < count && names; ++n)
        if (!sources[n].used) {
            initSource(&sources[n]);
            names[made++] = n;
        }
    if (made < count && names) {
        for (int i = 0; i < made; ++i) sources[names[i]].used = false;
        setError(AL_OUT_OF_MEMORY, "alGenSources");
    } else
        stats.sources_created += (unsigned)made;
    unlockAudio();
}
void linuxAlDeleteSources(int count, const unsigned *names) {
    lockAudio();
    ensureInit();
    if (count < 0) {
        setError(AL_INVALID_VALUE, "alDeleteSources");
        unlockAudio();
        return;
    }
    bool ok = true;
    for (int i = 0; i < count && names; ++i)
        if (!sourceByName(names[i])) {
            setError(AL_INVALID_NAME, "alDeleteSources");
            ok = false;
            break;
        }
    if (ok)
        for (int i = 0; i < count && names; ++i) {
            Source *s = sourceByName(names[i]);
            if (s) {
                clearQueue(s);
                s->used = false;
            }
        }
    unlockAudio();
}
unsigned char linuxAlIsSource(unsigned name) {
    lockAudio();
    unsigned char value = sourceByName(name) ? 1 : 0;
    unlockAudio();
    return value;
}

static int sourceCount(int parameter) {  // how many values the property holds: 0 when it is not a property
    switch (parameter) {
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION: return 3;
        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_REFERENCE_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_MAX_DISTANCE:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_CONE_OUTER_GAIN:
        case AL_LOOPING:
        case AL_SOURCE_RELATIVE:
        case AL_BUFFER:
        case AL_SOURCE_STATE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
        case AL_SOURCE_TYPE:
        case AL_DIRECT_CHANNELS_SOFT: return 1;
        default: return 0;
    }
}
static uint32_t frameBytes(const Buffer *b) { return (uint32_t)b->channels * b->bits / 8u; }
static uint32_t framesBefore(const Source *s, uint32_t index) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < index && i < s->count; ++i) total += buffers[s->queue[i]].frames;
    return total;
}
static const Buffer *firstBuffer(const Source *s) { return s->count ? &buffers[s->queue[0]] : NULL; }
static bool setOffset(Source *s, int kind, double value) {
    const Buffer *first = firstBuffer(s);
    if (!first || value < 0) return false;
    double frames = kind == AL_SEC_OFFSET ? value * first->frequency : (kind == AL_BYTE_OFFSET ? value / (frameBytes(first) ? frameBytes(first) : 1) : value);
    double accumulated = 0;
    for (uint32_t i = 0; i < s->count; ++i) {
        double length = buffers[s->queue[i]].frames;
        if (frames < accumulated + length) {
            s->current = i;
            s->sample = (uint32_t)(frames - accumulated);
            s->fraction = 0;
            s->processed = i;
            s->offset_pending = s->state != AL_PLAYING && s->state != AL_PAUSED;
            return true;
        }
        accumulated += length;
    }
    return false;
}
static uint32_t processedCount(const Source *s) {
    if (s->looping || s->state == AL_INITIAL) return 0;
    return s->state == AL_STOPPED ? s->count : s->processed;
}
static int sourceTypeOf(const Source *s) { return s->count == 0 ? AL_UNDETERMINED : s->type; }
// reads a property into out[0..2]; returns the number of values, 0 when unknown
static int readSource(Source *s, int parameter, double *out) {
    const Buffer *first = firstBuffer(s);
    switch (parameter) {
        case AL_PITCH: out[0] = s->pitch; return 1;
        case AL_GAIN: out[0] = s->gain; return 1;
        case AL_MIN_GAIN: out[0] = s->min_gain; return 1;
        case AL_MAX_GAIN: out[0] = s->max_gain; return 1;
        case AL_REFERENCE_DISTANCE: out[0] = s->reference_distance; return 1;
        case AL_ROLLOFF_FACTOR: out[0] = s->rolloff; return 1;
        case AL_MAX_DISTANCE: out[0] = s->max_distance; return 1;
        case AL_CONE_INNER_ANGLE: out[0] = s->cone_inner; return 1;
        case AL_CONE_OUTER_ANGLE: out[0] = s->cone_outer; return 1;
        case AL_CONE_OUTER_GAIN: out[0] = s->cone_outer_gain; return 1;
        case AL_LOOPING: out[0] = s->looping; return 1;
        case AL_SOURCE_RELATIVE: out[0] = s->relative; return 1;
        case AL_BUFFER: out[0] = s->count && s->current < s->count ? s->queue[s->current] : (s->count ? s->queue[s->count - 1] : 0); return 1;
        case AL_SOURCE_STATE: out[0] = s->state; return 1;
        case AL_BUFFERS_QUEUED: out[0] = s->count; return 1;
        case AL_BUFFERS_PROCESSED: out[0] = processedCount(s); return 1;
        case AL_SOURCE_TYPE: out[0] = sourceTypeOf(s); return 1;
        case AL_DIRECT_CHANNELS_SOFT: out[0] = s->direct_channels; return 1;
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET: {
            double frames = 0;
            if (s->state == AL_PLAYING || s->state == AL_PAUSED || s->offset_pending) frames = framesBefore(s, s->current) + s->sample;
            out[0] = parameter == AL_SAMPLE_OFFSET
                         ? frames
                         : (parameter == AL_SEC_OFFSET ? (first ? frames / first->frequency : 0) : frames * (first ? frameBytes(first) : 0));
            return 1;
        }
        case AL_POSITION: memcpy(out, (double[3]){s->position[0], s->position[1], s->position[2]}, sizeof(double) * 3); return 3;
        case AL_VELOCITY: memcpy(out, (double[3]){s->velocity[0], s->velocity[1], s->velocity[2]}, sizeof(double) * 3); return 3;
        case AL_DIRECTION: memcpy(out, (double[3]){s->direction[0], s->direction[1], s->direction[2]}, sizeof(double) * 3); return 3;
        default: return 0;
    }
}
static void stopSource(Source *s) {
    if (s->state == AL_PLAYING || s->state == AL_PAUSED) {
        s->state = AL_STOPPED;
        s->processed = s->count;
    }
    s->offset_pending = false;
}
static void applySource(unsigned name, Source *s, int parameter, const double *v, const char *where) {
    switch (parameter) {
        case AL_PITCH:
            if (v[0] <= 0) {
                setError(AL_INVALID_VALUE, where);
                return;
            }
            s->pitch = (float)v[0];
            return;
        case AL_GAIN:
            if (v[0] < 0) {
                setError(AL_INVALID_VALUE, where);
                return;
            }
            s->gain = (float)v[0];
            return;
        case AL_MIN_GAIN:
            if (v[0] < 0 || v[0] > 1) {
                setError(AL_INVALID_VALUE, where);
                return;
            }
            s->min_gain = (float)v[0];
            return;
        case AL_MAX_GAIN:
            if (v[0] < 0 || v[0] > 1) {
                setError(AL_INVALID_VALUE, where);
                return;
            }
            s->max_gain = (float)v[0];
            return;
        case AL_REFERENCE_DISTANCE:
            if (v[0] < 0) {
                setError(AL_INVALID_VALUE, where);
                return;
            }
            s->reference_distance = (float)v[0];
            return;
        case AL_ROLLOFF_FACTOR:
            if (v[0] < 0) {
                setError(AL_INVALID_VALUE, where);
                return;
            }
            s->rolloff = (float)v[0];
            return;
        case AL_MAX_DISTANCE:
            if (v[0] < 0) {
                setError(AL_INVALID_VALUE, where);
                return;
            }
            s->max_distance = (float)v[0];
            return;
        case AL_CONE_INNER_ANGLE: s->cone_inner = (float)v[0]; return;
        case AL_CONE_OUTER_ANGLE: s->cone_outer = (float)v[0]; return;
        case AL_CONE_OUTER_GAIN: s->cone_outer_gain = (float)v[0]; return;
        case AL_LOOPING: s->looping = v[0] != 0; return;
        case AL_SOURCE_RELATIVE: s->relative = v[0] != 0; return;
        case AL_DIRECT_CHANNELS_SOFT:
            if (v[0] != 0 && v[0] != 1 && v[0] != 2) {
                setError(AL_INVALID_VALUE, where);
                return;
            }
            s->direct_channels = (int)v[0];
            return;
        case AL_POSITION:
            for (int i = 0; i < 3; ++i) s->position[i] = (float)v[i];
            return;
        case AL_VELOCITY:
            for (int i = 0; i < 3; ++i) s->velocity[i] = (float)v[i];
            return;
        case AL_DIRECTION:
            for (int i = 0; i < 3; ++i) s->direction[i] = (float)v[i];
            return;
        case AL_BUFFER: {
            unsigned buffer = (unsigned)(long long)v[0];
            if (buffer && !bufferByName(buffer)) {
                setError(AL_INVALID_VALUE, where);
                return;
            }
            stopSource(s);  // a source that was playing stops: the game would otherwise be told off by a real OpenAL
            clearQueue(s);
            rewindSource(s);
            if (buffer) {
                s->queue[0] = buffer;
                s->count = 1;
                ++buffers[buffer].references;
                s->type = AL_STATIC;
            } else
                s->type = AL_UNDETERMINED;
            return;
        }
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            if (!setOffset(s, parameter, v[0])) setError(AL_INVALID_VALUE, where);
            return;
        case AL_SOURCE_STATE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_SOURCE_TYPE: setError(AL_INVALID_OPERATION, where); return;
        default:
            (void)name;
            setError(AL_INVALID_ENUM, where);
            return;
    }
}
static void setSource(unsigned name, int parameter, const double *v, int given,
                      const char *where) {  // given: values supplied, 0 = as many as the property takes
    lockAudio();
    ensureInit();
    last_parameter = parameter;
    Source *s = sourceByName(name);
    int need = sourceCount(parameter);
    if (!s)
        setError(AL_INVALID_NAME, where);
    else if (!need || (given && given != need))
        setError(AL_INVALID_ENUM, where);
    else if (!v)
        setError(AL_INVALID_VALUE, where);
    else
        applySource(name, s, parameter, v, where);
    unlockAudio();
}
void linuxAlSourcef(unsigned name, int parameter, float value) {
    double v[3] = {value, 0, 0};
    setSource(name, parameter, v, 1, "alSourcef");
}
void linuxAlSource3f(unsigned name, int parameter, float a, float b, float c) {
    double v[3] = {a, b, c};
    setSource(name, parameter, v, 3, "alSource3f");
}
void linuxAlSourcefv(unsigned name, int parameter, const float *values) {
    double v[3] = {0, 0, 0};
    int need = sourceCount(parameter);
    if (values)
        for (int i = 0; i < need && i < 3; ++i) v[i] = values[i];
    setSource(name, parameter, values ? v : NULL, 0, "alSourcefv");
}
void linuxAlSourcei(unsigned name, int parameter, int value) {
    double v[3] = {value, 0, 0};
    setSource(name, parameter, v, 1, "alSourcei");
}
void linuxAlSource3i(unsigned name, int parameter, int a, int b, int c) {
    double v[3] = {a, b, c};
    setSource(name, parameter, v, 3, "alSource3i");
}
void linuxAlSourceiv(unsigned name, int parameter, const int *values) {
    double v[3] = {0, 0, 0};
    int need = sourceCount(parameter);
    if (values)
        for (int i = 0; i < need && i < 3; ++i) v[i] = values[i];
    setSource(name, parameter, values ? v : NULL, 0, "alSourceiv");
}
static int getSource(unsigned name, int parameter, double *out, const char *where) {  // returns the count, 0 after an error
    lockAudio();
    ensureInit();
    Source *s = sourceByName(name);
    int count = 0;
    if (!s)
        setError(AL_INVALID_NAME, where);
    else {
        count = readSource(s, parameter, out);
        if (!count) setError(AL_INVALID_ENUM, where);
    }
    unlockAudio();
    return count;
}
static void badPointer(const char *where) {
    lockAudio();
    setError(AL_INVALID_VALUE, where);
    unlockAudio();
}
void linuxAlGetSourcef(unsigned name, int parameter, float *value) {
    double v[3];
    if (!value)
        badPointer("alGetSourcef");
    else if (getSource(name, parameter, v, "alGetSourcef") == 1)
        *value = (float)v[0];
}
void linuxAlGetSource3f(unsigned name, int parameter, float *a, float *b, float *c) {
    double v[3];
    if (!a || !b || !c)
        badPointer("alGetSource3f");
    else if (getSource(name, parameter, v, "alGetSource3f") == 3) {
        *a = (float)v[0];
        *b = (float)v[1];
        *c = (float)v[2];
    }
}
void linuxAlGetSourcefv(unsigned name, int parameter, float *values) {
    double v[3];
    int n;
    if (!values)
        badPointer("alGetSourcefv");
    else if ((n = getSource(name, parameter, v, "alGetSourcefv")) > 0)
        for (int i = 0; i < n; ++i) values[i] = (float)v[i];
}
static int roundInt(double value) { return (int)(value < 0 ? value - 0.5 : value + 0.5); }
void linuxAlGetSourcei(unsigned name, int parameter, int *value) {
    double v[3];
    if (!value)
        badPointer("alGetSourcei");
    else if (getSource(name, parameter, v, "alGetSourcei") == 1)
        *value = roundInt(v[0]);
}
void linuxAlGetSource3i(unsigned name, int parameter, int *a, int *b, int *c) {
    double v[3];
    if (!a || !b || !c)
        badPointer("alGetSource3i");
    else if (getSource(name, parameter, v, "alGetSource3i") == 3) {
        *a = roundInt(v[0]);
        *b = roundInt(v[1]);
        *c = roundInt(v[2]);
    }
}
void linuxAlGetSourceiv(unsigned name, int parameter, int *values) {
    double v[3];
    int n;
    if (!values)
        badPointer("alGetSourceiv");
    else if ((n = getSource(name, parameter, v, "alGetSourceiv")) > 0)
        for (int i = 0; i < n; ++i) values[i] = roundInt(v[i]);
}

static void playSource(Source *s) {
    ++stats.plays;
    switch (s->state) {
        case AL_PAUSED: s->state = AL_PLAYING; break;
        case AL_PLAYING:
            s->current = s->processed = 0;
            s->sample = s->fraction = 0;
            break;
        default:
            if (!s->offset_pending) {
                s->current = 0;
                s->sample = s->fraction = 0;
            }
            s->offset_pending = false;
            s->processed = s->current;
            s->state = AL_PLAYING;
            break;
    }
    if (!s->count) s->state = AL_STOPPED;  // nothing queued: the source ends at once
}
static void forEachSource(int count, const unsigned *names, const char *where, void (*action)(Source *)) {
    lockAudio();
    ensureInit();
    bool ok = count >= 0 && (count == 0 || names);
    if (!ok) setError(AL_INVALID_VALUE, where);
    for (int i = 0; ok && i < count; ++i)
        if (!sourceByName(names[i])) {
            setError(AL_INVALID_NAME, where);
            ok = false;
        }
    for (int i = 0; ok && i < count; ++i) action(sourceByName(names[i]));
    unlockAudio();
}
static void pauseSource(Source *s) {
    if (s->state == AL_PLAYING) s->state = AL_PAUSED;
}
void linuxAlSourcePlay(unsigned name) { forEachSource(1, &name, "alSourcePlay", playSource); }
void linuxAlSourceStop(unsigned name) { forEachSource(1, &name, "alSourceStop", stopSource); }
void linuxAlSourceRewind(unsigned name) { forEachSource(1, &name, "alSourceRewind", rewindSource); }
void linuxAlSourcePause(unsigned name) { forEachSource(1, &name, "alSourcePause", pauseSource); }
void linuxAlSourcePlayv(int count, const unsigned *names) { forEachSource(count, names, "alSourcePlayv", playSource); }
void linuxAlSourceStopv(int count, const unsigned *names) { forEachSource(count, names, "alSourceStopv", stopSource); }
void linuxAlSourceRewindv(int count, const unsigned *names) { forEachSource(count, names, "alSourceRewindv", rewindSource); }
void linuxAlSourcePausev(int count, const unsigned *names) { forEachSource(count, names, "alSourcePausev", pauseSource); }

void linuxAlSourceQueueBuffers(unsigned name, int count, const unsigned *names) {
    lockAudio();
    ensureInit();
    Source *s = sourceByName(name);
    bool ok = s != NULL;
    if (!s)
        setError(AL_INVALID_NAME, "alSourceQueueBuffers");
    else if (count < 0 || (count && !names)) {
        setError(AL_INVALID_VALUE, "alSourceQueueBuffers");
        ok = false;
    } else if (s->count + (unsigned)count > MAX_QUEUE) {
        setError(AL_OUT_OF_MEMORY, "alSourceQueueBuffers");
        ok = false;
    }
    for (int i = 0; ok && i < count; ++i)
        if (names[i] && !bufferByName(names[i])) {
            setError(AL_INVALID_NAME, "alSourceQueueBuffers");
            ok = false;
        }
    if (ok && s->type == AL_STATIC && s->count) {
        setError(AL_INVALID_OPERATION, "alSourceQueueBuffers");
        ok = false;
    }
    for (int i = 0; ok && i < count; ++i)
        if (names[i]) {
            s->queue[s->count++] = names[i];
            ++buffers[names[i]].references;
            ++stats.queued_buffers;
            if (stats.queued_buffers <= 12) trace("audio.source.queue source=%u buffer=%u queued=%u state=0x%x", name, names[i], s->count, (unsigned)s->state);
        }
    if (ok && count) s->type = AL_STREAMING;
    unlockAudio();
}
void linuxAlSourceUnqueueBuffers(unsigned name, int count, unsigned *names) {
    lockAudio();
    ensureInit();
    Source *s = sourceByName(name);
    if (!s)
        setError(AL_INVALID_NAME, "alSourceUnqueueBuffers");
    else if (count < 0 || (count && !names))
        setError(AL_INVALID_VALUE, "alSourceUnqueueBuffers");
    else if ((uint32_t)count > processedCount(s) && !(s->looping == false && (uint32_t)count <= s->count && s->state == AL_INITIAL))
        setError(AL_INVALID_VALUE, "alSourceUnqueueBuffers");
    else if (count) {
        uint32_t n = (uint32_t)count;
        for (uint32_t i = 0; i < n; ++i) {
            names[i] = s->queue[i];
            Buffer *b = bufferByName(names[i]);
            if (b && b->references) --b->references;
        }
        memmove(s->queue, s->queue + n, (s->count - n) * sizeof(uint32_t));
        s->count -= n;
        s->current = s->current >= n ? s->current - n : 0;
        s->processed = s->processed >= n ? s->processed - n : 0;
        stats.unqueued_buffers += n;
    }
    unlockAudio();
}

// ---- listener, state ----------------------------------------------------------------------------------------------------------
static void listenerSet(int parameter, const double *v, int given, const char *where) {
    lockAudio();
    ensureInit();
    switch (parameter) {
        case AL_GAIN:
            if (given && given != 1)
                setError(AL_INVALID_ENUM, where);
            else if (v[0] < 0)
                setError(AL_INVALID_VALUE, where);
            else
                listener.gain = (float)v[0];
            break;
        case AL_POSITION:
            if (given && given != 3)
                setError(AL_INVALID_ENUM, where);
            else
                for (int i = 0; i < 3; ++i) listener.position[i] = (float)v[i];
            break;
        case AL_VELOCITY:
            if (given && given != 3)
                setError(AL_INVALID_ENUM, where);
            else
                for (int i = 0; i < 3; ++i) listener.velocity[i] = (float)v[i];
            break;
        case AL_ORIENTATION:
            if (given)
                setError(AL_INVALID_ENUM, where);
            else
                for (int i = 0; i < 6; ++i) listener.orientation[i] = (float)v[i];
            break;
        default: setError(AL_INVALID_ENUM, where); break;
    }
    unlockAudio();
}
void linuxAlListenerf(int parameter, float value) {
    double v[6] = {value};
    listenerSet(parameter, v, 1, "alListenerf");
}
void linuxAlListener3f(int parameter, float a, float b, float c) {
    double v[6] = {a, b, c};
    listenerSet(parameter, v, 3, "alListener3f");
}
void linuxAlListenerfv(int parameter, const float *values) {
    double v[6] = {0};
    int count = parameter == AL_ORIENTATION ? 6 : (parameter == AL_GAIN ? 1 : 3);
    if (!values) {
        badPointer("alListenerfv");
        return;
    }
    for (int i = 0; i < count; ++i) v[i] = values[i];
    listenerSet(parameter, v, 0, "alListenerfv");
}
void linuxAlListeneri(int parameter, int value) {
    double v[6] = {value};
    listenerSet(parameter, v, 1, "alListeneri");
}
void linuxAlListener3i(int parameter, int a, int b, int c) {
    double v[6] = {a, b, c};
    listenerSet(parameter, v, 3, "alListener3i");
}
void linuxAlListeneriv(int parameter, const int *values) {
    double v[6] = {0};
    int count = parameter == AL_ORIENTATION ? 6 : (parameter == AL_GAIN ? 1 : 3);
    if (!values) {
        badPointer("alListeneriv");
        return;
    }
    for (int i = 0; i < count; ++i) v[i] = values[i];
    listenerSet(parameter, v, 0, "alListeneriv");
}
static int listenerGet(int parameter, double *out, const char *where) {
    lockAudio();
    ensureInit();
    int count = 0;
    switch (parameter) {
        case AL_GAIN:
            out[0] = listener.gain;
            count = 1;
            break;
        case AL_POSITION:
            for (int i = 0; i < 3; ++i) out[i] = listener.position[i];
            count = 3;
            break;
        case AL_VELOCITY:
            for (int i = 0; i < 3; ++i) out[i] = listener.velocity[i];
            count = 3;
            break;
        case AL_ORIENTATION:
            for (int i = 0; i < 6; ++i) out[i] = listener.orientation[i];
            count = 6;
            break;
        default: setError(AL_INVALID_ENUM, where); break;
    }
    unlockAudio();
    return count;
}
void linuxAlGetListenerf(int parameter, float *value) {
    double v[6];
    if (!value)
        badPointer("alGetListenerf");
    else if (listenerGet(parameter, v, "alGetListenerf") == 1)
        *value = (float)v[0];
}
void linuxAlGetListener3f(int parameter, float *a, float *b, float *c) {
    double v[6];
    if (!a || !b || !c)
        badPointer("alGetListener3f");
    else if (listenerGet(parameter, v, "alGetListener3f") == 3) {
        *a = (float)v[0];
        *b = (float)v[1];
        *c = (float)v[2];
    }
}
void linuxAlGetListenerfv(int parameter, float *values) {
    double v[6];
    int n;
    if (!values)
        badPointer("alGetListenerfv");
    else if ((n = listenerGet(parameter, v, "alGetListenerfv")) > 0)
        for (int i = 0; i < n; ++i) values[i] = (float)v[i];
}
void linuxAlGetListeneri(int parameter, int *value) {
    double v[6];
    if (!value)
        badPointer("alGetListeneri");
    else if (listenerGet(parameter, v, "alGetListeneri") == 1)
        *value = roundInt(v[0]);
}
void linuxAlGetListener3i(int parameter, int *a, int *b, int *c) {
    double v[6];
    if (!a || !b || !c)
        badPointer("alGetListener3i");
    else if (listenerGet(parameter, v, "alGetListener3i") == 3) {
        *a = roundInt(v[0]);
        *b = roundInt(v[1]);
        *c = roundInt(v[2]);
    }
}
void linuxAlGetListeneriv(int parameter, int *values) {
    double v[6];
    int n;
    if (!values)
        badPointer("alGetListeneriv");
    else if ((n = listenerGet(parameter, v, "alGetListeneriv")) > 0)
        for (int i = 0; i < n; ++i) values[i] = roundInt(v[i]);
}

// The one capability the client switches: AL_STOP_SOURCES_ON_DISCONNECT_SOFT (on by default). There is no device to lose here, so it is only remembered.
static bool stop_on_disconnect = true;
static void setCapability(int capability, bool value, const char *where) {
    lockAudio();
    ensureInit();
    last_parameter = capability;
    if (capability == AL_STOP_SOURCES_ON_DISCONNECT_SOFT)
        stop_on_disconnect = value;
    else
        setError(AL_INVALID_ENUM, where);
    unlockAudio();
}
void linuxAlEnable(int capability) { setCapability(capability, true, "alEnable"); }
void linuxAlDisable(int capability) { setCapability(capability, false, "alDisable"); }
unsigned char linuxAlIsEnabled(int capability) {
    lockAudio();
    ensureInit();
    last_parameter = capability;
    unsigned char value = 0;
    if (capability == AL_STOP_SOURCES_ON_DISCONNECT_SOFT)
        value = stop_on_disconnect;
    else
        setError(AL_INVALID_ENUM, "alIsEnabled");
    unlockAudio();
    return value;
}
static const char *const al_extensions = "AL_EXT_OFFSET AL_EXT_LINEAR_DISTANCE AL_EXT_EXPONENT_DISTANCE AL_EXT_FLOAT32";
static const char *const alc_extensions = "ALC_ENUMERATION_EXT ALC_ENUMERATE_ALL_EXT ALC_SOFT_reopen_device ALC_SOFT_system_events";
const char *linuxAlGetString(int parameter) {
    switch (parameter) {
        case AL_VENDOR: return "Horizon";
        case AL_VERSION: return "1.1 Horizon";
        case AL_RENDERER: return "Software mixer";
        case AL_EXTENSIONS: return al_extensions;
        case 0: return "";
        case AL_INVALID_NAME: return "Invalid Name";
        case AL_INVALID_ENUM: return "Invalid Enum";
        case AL_INVALID_VALUE: return "Invalid Value";
        case AL_INVALID_OPERATION: return "Invalid Operation";
        case AL_OUT_OF_MEMORY: return "Out of Memory";
        default:
            lockAudio();
            ensureInit();
            setError(AL_INVALID_ENUM, "alGetString");
            unlockAudio();
            return NULL;
    }
}
static bool stateValue(int parameter, double *value) {
    lockAudio();
    ensureInit();
    bool ok = true;
    switch (parameter) {
        case AL_DOPPLER_FACTOR: *value = doppler_factor; break;
        case AL_DOPPLER_VELOCITY: *value = doppler_velocity; break;
        case AL_SPEED_OF_SOUND: *value = speed_of_sound; break;
        case AL_DISTANCE_MODEL: *value = distance_model; break;
        default:
            setError(AL_INVALID_ENUM, "alGet");
            ok = false;
            break;
    }
    unlockAudio();
    return ok;
}
unsigned char linuxAlGetBoolean(int parameter) {
    double v = 0;
    return stateValue(parameter, &v) && v != 0;
}
int linuxAlGetInteger(int parameter) {
    double v = 0;
    stateValue(parameter, &v);
    return roundInt(v);
}
float linuxAlGetFloat(int parameter) {
    double v = 0;
    stateValue(parameter, &v);
    return (float)v;
}
double linuxAlGetDouble(int parameter) {
    double v = 0;
    stateValue(parameter, &v);
    return v;
}
void linuxAlGetBooleanv(int parameter, unsigned char *values) {
    if (values) values[0] = linuxAlGetBoolean(parameter);
}
void linuxAlGetIntegerv(int parameter, int *values) {
    if (values) values[0] = linuxAlGetInteger(parameter);
}
void linuxAlGetFloatv(int parameter, float *values) {
    if (values) values[0] = linuxAlGetFloat(parameter);
}
void linuxAlGetDoublev(int parameter, double *values) {
    if (values) values[0] = linuxAlGetDouble(parameter);
}
int linuxAlGetError(void) {
    lockAudio();
    int value = last_error;
    last_error = AL_NO_ERROR;
    unlockAudio();
    return value;
}
static bool listed(const char *list, const char *name) {
    size_t length = name ? strlen(name) : 0;
    for (const char *p = list; length && *p;) {
        while (*p == ' ') ++p;
        const char *end = p;
        while (*end && *end != ' ') ++end;
        if ((size_t)(end - p) == length && !strncmp(p, name, length)) return true;
        p = end;
    }
    return false;
}
unsigned char linuxAlIsExtensionPresent(const char *name) { return listed(al_extensions, name) ? 1 : 0; }
static const struct {
    const char *name;
    int value;
} enum_names[] = {{"AL_NONE", 0},
                  {"AL_FALSE", 0},
                  {"AL_TRUE", 1},
                  {"AL_SOURCE_RELATIVE", AL_SOURCE_RELATIVE},
                  {"AL_PITCH", AL_PITCH},
                  {"AL_POSITION", AL_POSITION},
                  {"AL_DIRECTION", AL_DIRECTION},
                  {"AL_VELOCITY", AL_VELOCITY},
                  {"AL_LOOPING", AL_LOOPING},
                  {"AL_BUFFER", AL_BUFFER},
                  {"AL_GAIN", AL_GAIN},
                  {"AL_ORIENTATION", AL_ORIENTATION},
                  {"AL_SOURCE_STATE", AL_SOURCE_STATE},
                  {"AL_INITIAL", AL_INITIAL},
                  {"AL_PLAYING", AL_PLAYING},
                  {"AL_PAUSED", AL_PAUSED},
                  {"AL_STOPPED", AL_STOPPED},
                  {"AL_BUFFERS_QUEUED", AL_BUFFERS_QUEUED},
                  {"AL_BUFFERS_PROCESSED", AL_BUFFERS_PROCESSED},
                  {"AL_FORMAT_MONO8", AL_FORMAT_MONO8},
                  {"AL_FORMAT_MONO16", AL_FORMAT_MONO16},
                  {"AL_FORMAT_STEREO8", AL_FORMAT_STEREO8},
                  {"AL_FORMAT_STEREO16", AL_FORMAT_STEREO16},
                  {"AL_FORMAT_MONO_FLOAT32", AL_FORMAT_MONO_FLOAT32},
                  {"AL_FORMAT_STEREO_FLOAT32", AL_FORMAT_STEREO_FLOAT32},
                  {"AL_FREQUENCY", AL_FREQUENCY},
                  {"AL_BITS", AL_BITS},
                  {"AL_CHANNELS", AL_CHANNELS},
                  {"AL_SIZE", AL_SIZE},
                  {"AL_NO_ERROR", 0},
                  {"AL_INVALID_NAME", AL_INVALID_NAME},
                  {"AL_INVALID_ENUM", AL_INVALID_ENUM},
                  {"AL_INVALID_VALUE", AL_INVALID_VALUE},
                  {"AL_INVALID_OPERATION", AL_INVALID_OPERATION},
                  {"AL_OUT_OF_MEMORY", AL_OUT_OF_MEMORY},
                  {"AL_DISTANCE_MODEL", AL_DISTANCE_MODEL},
                  {"AL_INVERSE_DISTANCE_CLAMPED", AL_INVERSE_DISTANCE_CLAMPED},
                  {"AL_SEC_OFFSET", AL_SEC_OFFSET},
                  {"AL_SAMPLE_OFFSET", AL_SAMPLE_OFFSET},
                  {"AL_BYTE_OFFSET", AL_BYTE_OFFSET},
                  {"ALC_FREQUENCY", ALC_FREQUENCY},
                  {"ALC_REFRESH", ALC_REFRESH},
                  {"ALC_SYNC", ALC_SYNC},
                  {"ALC_MONO_SOURCES", ALC_MONO_SOURCES},
                  {"ALC_STEREO_SOURCES", ALC_STEREO_SOURCES},
                  {"ALC_NO_ERROR", 0},
                  {"ALC_INVALID_DEVICE", ALC_INVALID_DEVICE},
                  {"ALC_INVALID_CONTEXT", ALC_INVALID_CONTEXT},
                  {"ALC_INVALID_ENUM", ALC_INVALID_ENUM},
                  {"ALC_INVALID_VALUE", ALC_INVALID_VALUE},
                  {"ALC_MAJOR_VERSION", ALC_MAJOR_VERSION},
                  {"ALC_MINOR_VERSION", ALC_MINOR_VERSION},
                  {"ALC_ATTRIBUTES_SIZE", ALC_ATTRIBUTES_SIZE},
                  {"ALC_ALL_ATTRIBUTES", ALC_ALL_ATTRIBUTES},
                  {"ALC_DEFAULT_DEVICE_SPECIFIER", ALC_DEFAULT_DEVICE_SPECIFIER},
                  {"ALC_DEVICE_SPECIFIER", ALC_DEVICE_SPECIFIER},
                  {"ALC_EXTENSIONS", ALC_EXTENSIONS},
                  {"ALC_DEFAULT_ALL_DEVICES_SPECIFIER", ALC_DEFAULT_ALL_DEVICES_SPECIFIER},
                  {"ALC_ALL_DEVICES_SPECIFIER", ALC_ALL_DEVICES_SPECIFIER},
                  {"ALC_CAPTURE_DEVICE_SPECIFIER", ALC_CAPTURE_DEVICE_SPECIFIER},
                  {"ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER", ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER},
                  {"ALC_CAPTURE_SAMPLES", ALC_CAPTURE_SAMPLES}};
int linuxAlGetEnumValue(const char *name) {
    for (size_t i = 0; name && i < sizeof(enum_names) / sizeof(enum_names[0]); ++i)
        if (!strcmp(enum_names[i].name, name)) return enum_names[i].value;
    return 0;
}
void linuxAlDopplerFactor(float value) {
    lockAudio();
    ensureInit();
    if (value < 0)
        setError(AL_INVALID_VALUE, "alDopplerFactor");
    else
        doppler_factor = value;
    unlockAudio();
}
void linuxAlDopplerVelocity(float value) {
    lockAudio();
    ensureInit();
    if (value < 0)
        setError(AL_INVALID_VALUE, "alDopplerVelocity");
    else
        doppler_velocity = value;
    unlockAudio();
}
void linuxAlSpeedOfSound(float value) {
    lockAudio();
    ensureInit();
    if (value <= 0)
        setError(AL_INVALID_VALUE, "alSpeedOfSound");
    else
        speed_of_sound = value;
    unlockAudio();
}
void linuxAlDistanceModel(int model) {
    lockAudio();
    ensureInit();
    if (model == 0 || (model >= AL_INVERSE_DISTANCE && model <= AL_EXPONENT_DISTANCE_CLAMPED))
        distance_model = model;
    else
        setError(AL_INVALID_ENUM, "alDistanceModel");
    unlockAudio();
}

// ---- ALC ----------------------------------------------------------------------------------------------------------------------
void *linuxAlcOpenDevice(const char *name) {
    (void)name;
    lockAudio();
    ensureInit();
    ++devices_open;
    ++stats.devices;
    trace("audio.device.open name=%s", name ? name : "(default)");
    unlockAudio();
    return device_object;
}
static void releaseEverything(void) {  // lock held
    for (unsigned i = 1; i <= MAX_SOURCES; ++i)
        if (sources[i].used) {
            clearQueue(&sources[i]);
            sources[i].used = false;
        }
    for (unsigned i = 1; i <= MAX_BUFFERS; ++i)
        if (buffers[i].used) releaseBuffer(&buffers[i]);
}
static void stopOutput(void) {  // lock NOT held: the output thread may be waiting for it
    lockAudio();
    bool running = output_running;
    output_running = false;
    void (*stop)(void) = output.stop;
    unlockAudio();
    if (running && stop) stop();
}
unsigned char linuxAlcCloseDevice(void *device) {
    if (device != device_object) {
        lockAudio();
        setAlcError(ALC_INVALID_DEVICE);
        unlockAudio();
        return 0;
    }
    lockAudio();
    bool last = devices_open && --devices_open == 0;
    unlockAudio();
    if (last) {
        stopOutput();
        lockAudio();
        releaseEverything();
        contexts_open = 0;
        context_current = false;
        unlockAudio();
    }
    return 1;
}
void *linuxAlcCreateContext(void *device, const int *attributes) {
    (void)attributes;
    lockAudio();
    ensureInit();
    if (device != device_object || !devices_open) {
        setAlcError(ALC_INVALID_DEVICE);
        unlockAudio();
        return NULL;
    }
    ++contexts_open;
    ++stats.contexts;
    bool (*start)(void) = output_running ? NULL : output.start;
    unlockAudio();
    if (start) {
        bool started = start();
        lockAudio();
        output_running = started;
        trace("audio.output.start=%s", started ? "OK" : "FAILED");
        unlockAudio();
    }
    return context_object;
}
unsigned char linuxAlcMakeContextCurrent(void *context) {
    lockAudio();
    if (context && context != context_object) {
        setAlcError(ALC_INVALID_CONTEXT);
        unlockAudio();
        return 0;
    }
    context_current = context != NULL;
    unlockAudio();
    return 1;
}
void linuxAlcProcessContext(void *context) { (void)context; }
void linuxAlcSuspendContext(void *context) { (void)context; }
void linuxAlcDestroyContext(void *context) {
    lockAudio();
    if (context != context_object || !contexts_open) {
        setAlcError(ALC_INVALID_CONTEXT);
        unlockAudio();
        return;
    }
    if (--contexts_open == 0) {
        context_current = false;
        for (unsigned i = 1; i <= MAX_SOURCES; ++i)
            if (sources[i].used) {
                clearQueue(&sources[i]);
                sources[i].used = false;
            }
    }
    unlockAudio();
}
void *linuxAlcGetCurrentContext(void) {
    lockAudio();
    void *value = context_current ? (void *)context_object : NULL;
    unlockAudio();
    return value;
}
void *linuxAlcGetContextsDevice(void *context) {
    (void)context;
    return device_object;
}
int linuxAlcGetError(void *device) {
    (void)device;
    lockAudio();
    int value = last_alc_error;
    last_alc_error = ALC_NO_ERROR;
    unlockAudio();
    return value;
}
unsigned char linuxAlcIsExtensionPresent(void *device, const char *name) {
    (void)device;
    return listed(alc_extensions, name) ? 1 : 0;
}
int linuxAlcGetEnumValue(void *device, const char *name) {
    (void)device;
    return linuxAlGetEnumValue(name);
}
const char *linuxAlcGetString(void *device, int parameter) {
    (void)device;
    switch (parameter) {
        case ALC_DEFAULT_DEVICE_SPECIFIER:
        case ALC_DEFAULT_ALL_DEVICES_SPECIFIER:
        case ALC_DEVICE_SPECIFIER: return "Switch audio";
        case ALC_ALL_DEVICES_SPECIFIER: return "Switch audio\0";  // a list: names end with a NUL and the list with a second one
        case ALC_EXTENSIONS: return alc_extensions;
        case ALC_CAPTURE_DEVICE_SPECIFIER:
        case ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER: return "";
        case ALC_NO_ERROR: return "No Error";
        case ALC_INVALID_DEVICE: return "Invalid Device";
        case ALC_INVALID_CONTEXT: return "Invalid Context";
        case ALC_INVALID_ENUM: return "Invalid Enum";
        case ALC_INVALID_VALUE: return "Invalid Value";
        default:
            lockAudio();
            setAlcError(ALC_INVALID_ENUM);
            unlockAudio();
            return NULL;
    }
}
void linuxAlcGetIntegerv(void *device, int parameter, int size, int *values) {
    (void)device;
    if (!values || size < 1) {
        lockAudio();
        setAlcError(ALC_INVALID_VALUE);
        unlockAudio();
        return;
    }
    switch (parameter) {
        case ALC_MAJOR_VERSION: values[0] = 1; break;
        case ALC_MINOR_VERSION: values[0] = 1; break;
        case ALC_FREQUENCY: values[0] = (int)LINUX_AUDIO_RATE; break;
        case ALC_REFRESH: values[0] = 50; break;
        case ALC_SYNC: values[0] = 0; break;
        case ALC_MONO_SOURCES: values[0] = (int)MAX_SOURCES - 1; break;
        case ALC_STEREO_SOURCES: values[0] = 1; break;
        case ALC_ATTRIBUTES_SIZE: values[0] = 11; break;
        case ALC_ALL_ATTRIBUTES: {
            static const int attributes[11] = {ALC_FREQUENCY,    (int)LINUX_AUDIO_RATE, ALC_REFRESH,        50, ALC_SYNC, 0,
                                               ALC_MONO_SOURCES, (int)MAX_SOURCES - 1,  ALC_STEREO_SOURCES, 1,  0};
            for (int i = 0; i < size && i < 11; ++i) values[i] = attributes[i];
            break;
        }
        case ALC_CAPTURE_SAMPLES: values[0] = 0; break;
        default:
            lockAudio();
            setAlcError(ALC_INVALID_ENUM);
            unlockAudio();
            break;
    }
}
void *linuxAlcCaptureOpenDevice(const char *name, unsigned frequency, int format, int size) {
    (void)name;
    (void)frequency;
    (void)format;
    (void)size;
    lockAudio();
    setAlcError(ALC_INVALID_VALUE);
    unlockAudio();
    return NULL;
}
unsigned char linuxAlcCaptureCloseDevice(void *device) {
    (void)device;
    return 0;
}
void linuxAlcCaptureStart(void *device) { (void)device; }
void linuxAlcCaptureStop(void *device) { (void)device; }
void linuxAlcCaptureSamples(void *device, void *buffer, int samples) {
    (void)device;
    (void)buffer;
    (void)samples;
}
unsigned char linuxAlcReopenDeviceSOFT(void *device, const char *name, const int *attributes) {
    (void)attributes;
    lockAudio();
    bool ok = device == device_object;
    if (!ok) setAlcError(ALC_INVALID_DEVICE);
    static unsigned reopens;
    if (reopens++ < 3) trace("audio.device.reopen name=%s ok=%d (further calls are not logged)", name ? name : "(default)", ok);
    unlockAudio();
    return ok ? 1 : 0;
}
unsigned char linuxAlcEventControlSOFT(int count, const int *events, unsigned char enable) {
    (void)events;
    (void)enable;
    return count >= 0 ? 1 : 0;
}
void linuxAlcEventCallbackSOFT(void *callback, void *user) {
    lockAudio();
    event_callback = callback;
    event_user = user;
    unlockAudio();
}

// ---- mixer --------------------------------------------------------------------------------------------------------------------
static inline void frameAt(const Source *s, uint32_t index, uint32_t sample, float *left, float *right) {
    // The frame `sample` frames into queue entry `index`, running on into the next entries (and round to the first when the source loops).
    uint32_t guard = s->count + 2;
    const Buffer *b = &buffers[s->queue[index]];
    while (sample >= b->frames) {
        if (!guard--) {
            *left = *right = 0;
            return;
        }
        sample -= b->frames;
        if (++index >= s->count) {
            if (!s->looping) {
                *left = *right = 0;
                return;
            }
            index = 0;
        }
        b = &buffers[s->queue[index]];
    }
    if (b->channels == 2) {
        *left = b->data[sample * 2];
        *right = b->data[sample * 2 + 1];
    } else
        *left = *right = b->data[sample];
}
// Moves past exhausted entries; false once the source has ended (and is then STOPPED).
static bool settle(Source *s) {
    uint32_t guard = s->count * 2 + 2;
    while (s->current < s->count && s->sample >= buffers[s->queue[s->current]].frames) {
        if (!guard--) {
            s->state = AL_STOPPED;
            s->processed = s->count;
            return false;
        }
        s->sample -= buffers[s->queue[s->current]].frames;
        ++s->current;
        if (!s->looping) s->processed = s->current;
        if (s->current >= s->count) {
            if (s->looping) {
                s->current = 0;
                s->processed = 0;
            } else {
                s->state = AL_STOPPED;
                s->processed = s->count;
                return false;
            }
        }
    }
    return s->current < s->count;
}
static float attenuation(const Source *s, float distance) {
    float reference = s->reference_distance, maximum = s->max_distance, rolloff = s->rolloff;
    int model = distance_model;
    bool clamped = model == AL_INVERSE_DISTANCE_CLAMPED || model == AL_LINEAR_DISTANCE_CLAMPED || model == AL_EXPONENT_DISTANCE_CLAMPED;
    if (clamped) {
        if (distance < reference) distance = reference;
        if (distance > maximum) distance = maximum;
    }
    float gain = 1;
    switch (model) {
        case AL_INVERSE_DISTANCE:
        case AL_INVERSE_DISTANCE_CLAMPED: {
            float d = reference + rolloff * (distance - reference);
            gain = d > 0 ? reference / d : 1;
            break;
        }
        case AL_LINEAR_DISTANCE:
        case AL_LINEAR_DISTANCE_CLAMPED: gain = maximum > reference ? 1 - rolloff * (distance - reference) / (maximum - reference) : 1; break;
        case AL_EXPONENT_DISTANCE:
        case AL_EXPONENT_DISTANCE_CLAMPED: gain = reference > 0 && distance > 0 ? powf(distance / reference, -rolloff) : 1; break;
        default: break;
    }
    return gain < 0 ? 0 : (gain > 1e6f ? 1e6f : gain);
}
// Left and right gains of a source: distance attenuation for every source, panning for mono ones.
static void spatialize(const Source *s, bool mono, float *left, float *right) {
    float p[3] = {s->position[0], s->position[1], s->position[2]};
    if (!s->relative)
        for (int i = 0; i < 3; ++i) p[i] -= listener.position[i];
    float distance = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    float gain = s->gain * attenuation(s, distance);
    if (gain < s->min_gain) gain = s->min_gain;
    if (gain > s->max_gain) gain = s->max_gain;
    gain *= listener.gain;
    float pan = 0;
    if (mono && distance > 1e-6f) {
        const float *at = listener.orientation, *up = listener.orientation + 3;
        float right_axis[3] = {at[1] * up[2] - at[2] * up[1], at[2] * up[0] - at[0] * up[2], at[0] * up[1] - at[1] * up[0]};
        float length = sqrtf(right_axis[0] * right_axis[0] + right_axis[1] * right_axis[1] + right_axis[2] * right_axis[2]);
        if (length > 1e-6f) {
            pan = (p[0] * right_axis[0] + p[1] * right_axis[1] + p[2] * right_axis[2]) / (length * distance);
            if (pan > 1) pan = 1;
            if (pan < -1) pan = -1;
        }
    }
    if (mono) {
        float angle = (pan + 1) * 0.78539816f;
        *left = gain * cosf(angle);
        *right = gain * sinf(angle);
    } else
        *left = *right = gain;
}
static void mixSource(Source *s, float *accumulator, unsigned frames) {
    if (!settle(s)) return;
    bool mono = buffers[s->queue[s->current]].channels == 1;
    float gain_left, gain_right;
    spatialize(s, mono, &gain_left, &gain_right);
    const float scale = 1.0f / 4294967296.0f;
    for (unsigned f = 0; f < frames; ++f) {
        if (s->state != AL_PLAYING || !settle(s)) break;
        const Buffer *b = &buffers[s->queue[s->current]];
        float l0, r0, l1, r1;
        frameAt(s, s->current, s->sample, &l0, &r0);
        frameAt(s, s->current, s->sample + 1, &l1, &r1);
        float t = (float)s->fraction * scale;
        accumulator[f * 2] += (l0 + (l1 - l0) * t) * gain_left;
        accumulator[f * 2 + 1] += (r0 + (r1 - r0) * t) * gain_right;
        uint64_t step = (uint64_t)((double)b->frequency * s->pitch / (double)LINUX_AUDIO_RATE * 4294967296.0);
        uint64_t next = (uint64_t)s->fraction + step;
        s->sample += (uint32_t)(next >> 32);
        s->fraction = (uint32_t)next;
    }
    settle(s);
}
void linuxAudioMix(int16_t *interleaved, unsigned frames) {
    static float accumulator[MAX_BLOCK * 2];
    if (!interleaved) return;
    if (frames > MAX_BLOCK) {
        memset(interleaved + MAX_BLOCK * 2, 0, (size_t)(frames - MAX_BLOCK) * 4);
        frames = MAX_BLOCK;
    }
    lockAudio();
    memset(accumulator, 0, (size_t)frames * 2 * sizeof(float));
    unsigned playing = 0;
    if (initialized)
        for (unsigned i = 1; i <= MAX_SOURCES; ++i)
            if (sources[i].used && sources[i].state == AL_PLAYING) {
                ++playing;
                mixSource(&sources[i], accumulator, frames);
            }
    int peak = stats.peak;
    for (unsigned i = 0; i < frames * 2; ++i) {
        float v = accumulator[i];
        int16_t out = v >= 32767.0f ? 32767 : (v <= -32768.0f ? -32768 : (int16_t)v);
        interleaved[i] = out;
        int magnitude = out < 0 ? -out : out;
        if (magnitude > peak) peak = magnitude;
    }
    stats.peak = peak;
    if (playing > stats.max_playing) stats.max_playing = playing;
    stats.mixed_frames += frames;
    ++stats.mix_calls;
    unlockAudio();
}

void linuxAudioReset(void) {
    stopOutput();
    lockAudio();
    log_lines = 0;  // the summary is always written, however much was logged before
    trace(
        "audio.summary devices=%u contexts=%u buffers=%u sources=%u buffer_data=%u buffer_bytes=%llu plays=%u queued=%u unqueued=%u errors=%u max_playing=%u peak=%d mix_calls=%llu mixed_frames=%llu",
        stats.devices, stats.contexts, stats.buffers_created, stats.sources_created, stats.buffer_data_calls, (unsigned long long)stats.buffer_bytes,
        stats.plays, stats.queued_buffers, stats.unqueued_buffers, stats.errors, stats.max_playing, stats.peak, (unsigned long long)stats.mix_calls,
        (unsigned long long)stats.mixed_frames);
    releaseEverything();
    memset(&stats, 0, sizeof(stats));
    memset(&listener, 0, sizeof(listener));
    initialized = false;
    devices_open = contexts_open = 0;
    context_current = false;
    last_error = last_alc_error = 0;
    event_callback = event_user = NULL;
    unlockAudio();
}
