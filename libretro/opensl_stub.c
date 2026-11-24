/*
 * Minimal OpenSL ES stub: capture Enqueue() PCM into a ring buffer for
 * libretro.
 *
 * Object layout matches core audio_out.c method() helper:
 *   first word of object = vptr (pointer to function table)
 *   method(obj, byte_offset) loads *(vptr + byte_offset)
 */

#include "libretro_internal.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform/android/audio.h"

#define RING_FRAMES (SEEDLESS_AUDIO_RATE / 2)

typedef int32_t SLresult;
typedef uint32_t SLuint32;
typedef uint32_t SLboolean;

typedef void (*sl_bq_cb)(void *caller, void *context);

struct SLInterfaceID_ {
    uint32_t data[4];
};

static const struct SLInterfaceID_ iid_engine = {{1, 0, 0, 0}};
static const struct SLInterfaceID_ iid_bufferqueue = {{2, 0, 0, 0}};
static const struct SLInterfaceID_ iid_play = {{3, 0, 0, 0}};
static const struct SLInterfaceID_ iid_volume = {{4, 0, 0, 0}};
static const struct SLInterfaceID_ iid_record = {{5, 0, 0, 0}};
static const struct SLInterfaceID_ iid_android_sbq = {{6, 0, 0, 0}};

const SLInterfaceID SL_IID_ENGINE = &iid_engine;
const SLInterfaceID SL_IID_BUFFERQUEUE = &iid_bufferqueue;
const SLInterfaceID SL_IID_PLAY = &iid_play;
const SLInterfaceID SL_IID_VOLUME = &iid_volume;
const SLInterfaceID SL_IID_RECORD = &iid_record;
const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &iid_android_sbq;

static int16_t g_ring[RING_FRAMES * 2];
static size_t g_r_r, g_r_w;
static pthread_mutex_t g_ring_mu = PTHREAD_MUTEX_INITIALIZER;

void seedless_audio_reset(void)
{
    pthread_mutex_lock(&g_ring_mu);
    g_r_r = g_r_w = 0;
    pthread_mutex_unlock(&g_ring_mu);
}

static void ring_write_bytes(const void *data, uint32_t bytes)
{
    const int16_t *in = (const int16_t *)data;
    size_t samples = bytes / sizeof(int16_t);
    size_t frames = samples / 2;
    size_t i;

    pthread_mutex_lock(&g_ring_mu);
    for (i = 0; i < frames; i++) {
        size_t next = (g_r_w + 1) % RING_FRAMES;
        if (next == g_r_r)
            g_r_r = (g_r_r + 1) % RING_FRAMES;
        g_ring[g_r_w * 2 + 0] = in[i * 2 + 0];
        g_ring[g_r_w * 2 + 1] = in[i * 2 + 1];
        g_r_w = next;
    }
    if (samples & 1u) {
        size_t next = (g_r_w + 1) % RING_FRAMES;
        int16_t s = in[samples - 1];
        if (next == g_r_r)
            g_r_r = (g_r_r + 1) % RING_FRAMES;
        g_ring[g_r_w * 2 + 0] = s;
        g_ring[g_r_w * 2 + 1] = s;
        g_r_w = next;
    }
    pthread_mutex_unlock(&g_ring_mu);
}

size_t seedless_audio_frames_available(void)
{
    size_t n;
    pthread_mutex_lock(&g_ring_mu);
    n = (g_r_w + RING_FRAMES - g_r_r) % RING_FRAMES;
    pthread_mutex_unlock(&g_ring_mu);
    return n;
}

size_t seedless_audio_read(int16_t *out, size_t max_frames)
{
    size_t got = 0;
    pthread_mutex_lock(&g_ring_mu);
    while (got < max_frames && g_r_r != g_r_w) {
        out[got * 2 + 0] = g_ring[g_r_r * 2 + 0];
        out[got * 2 + 1] = g_ring[g_r_r * 2 + 1];
        g_r_r = (g_r_r + 1) % RING_FRAMES;
        got++;
    }
    pthread_mutex_unlock(&g_ring_mu);
    return got;
}

/* Release OpenSL buffer-queue backpressure after libretro consumes PCM.
 * buffer_samples counts int16 values (2 per stereo frame). */
void seedless_audio_mark_consumed(size_t frames)
{
    static size_t s_acc;
    uint32_t samples_per_buf;
    uint32_t frames_per_buf;

    if (frames == 0 || PLATFORM_AUDIO->active == 0)
        return;

    samples_per_buf = PLATFORM_AUDIO->buffer_samples;
    if (samples_per_buf < 2)
        return;
    frames_per_buf = samples_per_buf / 2u;

    s_acc += frames;
    while (s_acc >= frames_per_buf) {
        s_acc -= frames_per_buf;
        if (PLATFORM_AUDIO->queued > 0)
            PLATFORM_AUDIO->queued--;
        else
            break;
    }
}

typedef struct sl_obj sl_obj_t;

/* Captured samples, tagged with the buffer the core handed to enqueue.
 * audio.c ping-pongs PLATFORM_AUDIO->mic_buffers through the recorder queue:
 * each frame it reads mic_buffers[mic_slot] and re-enqueues that pointer, so
 * the recorder must write the live PCM into whichever pointer it was given,
 * then report the buffer as ready via the callback. A shared ring that the
 * core never reads would leave mic_buffers[] silent. */
#define MIC_RING_FRAMES 22050
static int16_t g_mic_ring[MIC_RING_FRAMES];
static size_t g_mic_r, g_mic_w;
static pthread_mutex_t g_mic_mu = PTHREAD_MUTEX_INITIALIZER;
static sl_bq_cb g_mic_cb;
static void *g_mic_cb_ctx;
static int g_mic_recording;
static uint8_t *g_mic_sink;      /* buffer the core is waiting to receive */
static uint32_t g_mic_sink_bytes;

void seedless_mic_reset(void)
{
    pthread_mutex_lock(&g_mic_mu);
    g_mic_r = g_mic_w = 0;
    g_mic_sink = NULL;
    g_mic_sink_bytes = 0;
    pthread_mutex_unlock(&g_mic_mu);
}

void seedless_mic_feed(const int16_t *samples, size_t count)
{
    size_t i;

    if (!samples || !count)
        return;
    pthread_mutex_lock(&g_mic_mu);
    for (i = 0; i < count; i++) {
        g_mic_ring[g_mic_w] = samples[i];
        g_mic_w = (g_mic_w + 1) % MIC_RING_FRAMES;
        if (g_mic_w == g_mic_r)
            g_mic_r = (g_mic_r + 1) % MIC_RING_FRAMES;
    }
    pthread_mutex_unlock(&g_mic_mu);
}

/* Fill the buffer the core enqueued from the captured ring, then report it
 * ready. Runs from the frontend frame, never reentrant with the core's own
 * enqueue, so mic_capture_queue_callback's mic_total accounting stays sane. */
void seedless_mic_service(void)
{
    uint8_t *sink;
    uint32_t bytes;
    int16_t *out;
    size_t samples, i;
    sl_bq_cb cb;
    void *ctx;

    pthread_mutex_lock(&g_mic_mu);
    sink = g_mic_sink;
    bytes = g_mic_sink_bytes;
    g_mic_sink = NULL;
    g_mic_sink_bytes = 0;
    cb = g_mic_cb;
    ctx = g_mic_cb_ctx;
    if (sink && bytes) {
        out = (int16_t *)sink;
        samples = bytes / 2;
        for (i = 0; i < samples; i++) {
            if (g_mic_r != g_mic_w) {
                out[i] = g_mic_ring[g_mic_r];
                g_mic_r = (g_mic_r + 1) % MIC_RING_FRAMES;
            } else {
                out[i] = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_mic_mu);

    if (sink && bytes && g_mic_recording && cb)
        cb(ctx, NULL);
}

struct sl_obj {
    const void *const *vptr; /* MUST be first: COM/Itf vtable pointer */
    sl_bq_cb callback;
    void *callback_ctx;
    sl_obj_t *engine_itf;
    sl_obj_t *output_mix;
    sl_obj_t *player;
    sl_obj_t *play_itf;
    sl_obj_t *bq_itf;
    sl_obj_t *vol_itf;
    sl_obj_t *record_itf;
};

static sl_obj_t g_engine;

static sl_obj_t *sl_new(const void *const *vtbl)
{
    sl_obj_t *o = (sl_obj_t *)calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->vptr = vtbl;
    return o;
}

static SLresult sl_ok(void)
{
    return 0;
}

static SLresult obj_realize(void *self, SLboolean async)
{
    (void)self;
    (void)async;
    return 0;
}

static SLresult obj_resume(void *self, SLboolean async)
{
    (void)self;
    (void)async;
    return 0;
}

static SLresult obj_get_state(void *self, SLuint32 *state)
{
    (void)self;
    if (state)
        *state = 1;
    return 0;
}

static SLresult bq_enqueue(void *self, const void *buffer, SLuint32 size)
{
    (void)self;
    /* Only capture PCM. queued is owned by feed_frame / mark_consumed
     * (real OpenSL decrements via RegisterCallback after the buffer plays). */
    /* A state load stops playback and then restarts it, and the restart
     * primes the queue with silent buffers before it clears `closed`. With a
     * real AudioTrack those buffers keep the device fed; here they would be
     * consumed as the first audio after the resume, delaying the restored
     * game's PCM by up to the core's whole queue (about half a second for a
     * large buffer mode). The ring was already cleared when playback
     * stopped, so drop them instead. */
    if (PLATFORM_AUDIO->closed != 0)
        return 0;
    if (buffer && size)
        ring_write_bytes(buffer, size);
    return 0;
}

static SLresult bq_clear(void *self)
{
    (void)self;
    seedless_audio_reset();
    PLATFORM_AUDIO->queued = 0;
    return 0;
}

static SLresult bq_get_state(void *self, void *state)
{
    (void)self;
    (void)state;
    return 0;
}

static SLresult bq_register_callback(void *self, void *cb, void *ctx)
{
    sl_obj_t *o = (sl_obj_t *)self;
    o->callback = (sl_bq_cb)cb;
    o->callback_ctx = ctx;
    return 0;
}

static SLresult play_set_state(void *self, SLuint32 state)
{
    (void)self;
    (void)state;
    return 0;
}

static SLresult vol_set(void *self, int16_t millibel)
{
    (void)self;
    (void)millibel;
    return 0;
}

static const void *const bq_vtbl[] = {
    (const void *)bq_enqueue,
    (const void *)bq_clear,
    (const void *)bq_get_state,
    (const void *)bq_register_callback,
};

static const void *const play_vtbl[] = {
    (const void *)play_set_state,
    (const void *)sl_ok,
};

static const void *const vol_vtbl[] = {
    (const void *)vol_set,
};

static SLresult engine_create_output_mix(void *self, void *pMix, SLuint32 n,
                                         void *ids, void *req);
static SLresult engine_create_audio_player(void *self, void *pPlayer, void *pSrc,
                                           void *pSnk, SLuint32 n, void *ids,
                                           void *req);
static SLresult obj_get_interface(void *self, SLInterfaceID iid, void *out);

static void obj_destroy(void *self)
{
    (void)self;
}

static const void *const obj_vtbl[] = {
    (const void *)obj_realize,
    (const void *)obj_resume,
    (const void *)obj_get_state,
    (const void *)obj_get_interface,
    NULL, /* RegisterCallback */
    NULL, /* AbortAsync */
    (const void *)obj_destroy,
};

static SLresult record_set_state(void *self, SLuint32 state)
{
    (void)self;
    g_mic_recording = state != 0;
    return 0;
}

static SLresult record_get_state(void *self, SLuint32 *state)
{
    (void)self;
    if (state)
        *state = g_mic_recording ? 1u : 0u;
    return 0;
}

static const void *const record_vtbl[] = {
    (const void *)record_set_state,
    (const void *)record_get_state,
    (const void *)sl_ok,
    (const void *)sl_ok,
};

static SLresult mic_bq_enqueue(void *self, const void *buffer, SLuint32 size)
{
    (void)self;
    if (buffer && size) {
        pthread_mutex_lock(&g_mic_mu);
        g_mic_sink = (uint8_t *)buffer;
        g_mic_sink_bytes = size;
        pthread_mutex_unlock(&g_mic_mu);
    }
    return 0;
}

static SLresult mic_bq_clear(void *self)
{
    (void)self;
    return 0;
}

static SLresult mic_bq_get_state(void *self, void *state)
{
    (void)self;
    (void)state;
    return 0;
}

static SLresult mic_bq_register_callback(void *self, void *cb, void *ctx)
{
    (void)self;
    g_mic_cb = (sl_bq_cb)cb;
    g_mic_cb_ctx = ctx;
    return 0;
}

static const void *const mic_bq_vtbl[] = {
    (const void *)mic_bq_enqueue,
    (const void *)mic_bq_clear,
    (const void *)mic_bq_get_state,
    (const void *)mic_bq_register_callback,
};

static sl_obj_t *g_recorder;
static sl_obj_t *g_record_itf;
static sl_obj_t *g_recorder_queue;

static SLresult engine_create_audio_recorder(void *self, void *pRec, void *a,
                                             void *b, SLuint32 n, void *ids,
                                             void *req)
{
    (void)self;
    (void)a;
    (void)b;
    (void)n;
    (void)ids;
    (void)req;

    if (!g_recorder) {
        g_recorder = sl_new(obj_vtbl);
        g_record_itf = sl_new(record_vtbl);
        g_recorder_queue = sl_new(mic_bq_vtbl);
        if (!g_recorder || !g_record_itf || !g_recorder_queue)
            return -1;
        g_recorder->record_itf = g_record_itf;
        g_recorder->bq_itf = g_recorder_queue;
    }
    if (pRec)
        *(void **)pRec = g_recorder;

    /* Fallback path for audio.c's sound_disabled sweep; the primary feed goes
     * through the buffer queue above. */
    PLATFORM_AUDIO->mic_samples = g_mic_ring;
    PLATFORM_AUDIO->mic_sample_index = 0;
    return 0;
}

static const void *const engine_itf_vtbl[] = {
    NULL,
    NULL,
    (const void *)engine_create_audio_player,   /* offset 16 */
    (const void *)engine_create_audio_recorder, /* offset 24 */
    NULL,
    NULL,
    NULL,
    (const void *)engine_create_output_mix,     /* offset 56 */
};

static SLresult engine_create_output_mix(void *self, void *pMix, SLuint32 n,
                                         void *ids, void *req)
{
    (void)self;
    (void)n;
    (void)ids;
    (void)req;
    if (!g_engine.output_mix) {
        g_engine.output_mix = sl_new(obj_vtbl);
        if (!g_engine.output_mix)
            return -1;
    }
    *(void **)pMix = g_engine.output_mix;
    return 0;
}

static SLresult engine_create_audio_player(void *self, void *pPlayer, void *pSrc,
                                           void *pSnk, SLuint32 n, void *ids,
                                           void *req)
{
    (void)self;
    (void)pSrc;
    (void)pSnk;
    (void)n;
    (void)ids;
    (void)req;

    if (!g_engine.player) {
        g_engine.player = sl_new(obj_vtbl);
        g_engine.play_itf = sl_new(play_vtbl);
        g_engine.bq_itf = sl_new(bq_vtbl);
        g_engine.vol_itf = sl_new(vol_vtbl);
        if (!g_engine.player || !g_engine.play_itf || !g_engine.bq_itf ||
            !g_engine.vol_itf)
            return -1;
        g_engine.player->play_itf = g_engine.play_itf;
        g_engine.player->bq_itf = g_engine.bq_itf;
        g_engine.player->vol_itf = g_engine.vol_itf;
    }
    *(void **)pPlayer = g_engine.player;
    return 0;
}

static SLresult obj_get_interface(void *self, SLInterfaceID iid, void *out)
{
    sl_obj_t *o = (sl_obj_t *)self;

    if (!out)
        return -1;

    if (iid == SL_IID_ENGINE) {
        if (!g_engine.engine_itf) {
            g_engine.engine_itf = sl_new(engine_itf_vtbl);
            if (!g_engine.engine_itf)
                return -1;
        }
        *(void **)out = g_engine.engine_itf;
        return 0;
    }
    if (iid == SL_IID_PLAY) {
        *(void **)out = o->play_itf ? o->play_itf : g_engine.play_itf;
        return 0;
    }
    if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
        *(void **)out = o->bq_itf ? o->bq_itf : g_engine.bq_itf;
        return 0;
    }
    if (iid == SL_IID_VOLUME) {
        *(void **)out = o->vol_itf ? o->vol_itf : g_engine.vol_itf;
        return 0;
    }
    if (iid == SL_IID_RECORD) {
        if (!o->record_itf)
            return -1;
        *(void **)out = o->record_itf;
        return 0;
    }
    return -1;
}

SLresult slCreateEngine(void *pEngine, SLuint32 numOptions, void *pOptions,
                        SLuint32 numInterfaces, void *pInterfaceIds,
                        void *pRequired)
{
    (void)numOptions;
    (void)pOptions;
    (void)numInterfaces;
    (void)pInterfaceIds;
    (void)pRequired;

    memset(&g_engine, 0, sizeof g_engine);
    g_engine.vptr = obj_vtbl;
    seedless_audio_reset();

    if (!pEngine)
        return -1;
    *(void **)pEngine = &g_engine;
    return 0;
}
