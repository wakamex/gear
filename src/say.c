#include "say.h"
#include <SDL3/SDL.h>
#include "gs_mix.h"
#include "gs_stream.h"
#include "pocket.h"

enum { POCKET_RATE = 24000, SCRATCH = 1024 };

typedef struct line { struct line *next; char *text; } line;

struct gear_say {
    struct pt_context *ctx;
    struct pt_voice *voice;
    gs_stream *stream;
    SDL_AudioStream *conv;  // Pocket's 24 kHz mono to the mix's rate in stereo
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *wake;
    line *queue;
    bool generating, quit;
    SDL_AtomicInt generation;  // advanced by gear_say_stop; synthesis of an older one gives up
    double first_audio, audio;
    int rate;
    // The player, on the audio thread: what it plays, averaged down to 16 kHz for the canceller.
    gs_voice *echo;
    float scratch[SCRATCH * 2], echo_frame[160], echo_sum;
    int echo_fill, echo_count;
};

typedef struct { gear_say *s; int generation; Uint64 start; bool heard; long samples; } job;

static void play(gear_say *s, int generation) {
    float buf[4096];
    for (int got; (got = SDL_GetAudioStreamData(s->conv, buf, sizeof buf)) > 0;) {
        if (SDL_GetAtomicInt(&s->generation) != generation) return;
        gs_stream_write(s->stream, buf, got / (int)(2 * sizeof(float)));
    }
    // A stop that came while this was being written drops it too.
    if (SDL_GetAtomicInt(&s->generation) != generation) gs_stream_flush(s->stream);
}

static bool on_chunk(const float *samples, int n, void *user) {
    job *j = user;
    gear_say *s = j->s;
    if (SDL_GetAtomicInt(&s->generation) != j->generation) return false;
    if (!j->heard && n > 0) {
        j->heard = true;
        SDL_LockMutex(s->lock);
        s->first_audio = (double)(SDL_GetTicksNS() - j->start) / 1e9;
        SDL_UnlockMutex(s->lock);
    }
    j->samples += n;
    SDL_PutAudioStreamData(s->conv, samples, n * (int)sizeof(float));
    play(s, j->generation);
    return SDL_GetAtomicInt(&s->generation) == j->generation;
}

static int run(void *user) {
    gear_say *s = user;
    SDL_LockMutex(s->lock);
    for (;;) {
        while (!s->queue && !s->quit) SDL_WaitCondition(s->wake, s->lock);
        if (s->quit) break;
        line *l = s->queue;
        s->queue = l->next;
        s->generating = true;
        job j = { s, SDL_GetAtomicInt(&s->generation), SDL_GetTicksNS(), false, 0 };
        SDL_UnlockMutex(s->lock);
        struct pt_tts_params p;
        pt_tts_default_params(&p);
        p.text = l->text;
        p.voice = s->voice;
        p.on_chunk = on_chunk;
        p.user_data = &j;
        if (pt_synthesize(s->ctx, &p, NULL) && SDL_GetAtomicInt(&s->generation) == j.generation) SDL_Log("gear: synthesis failed: %s", pt_last_error());
        if (SDL_GetAtomicInt(&s->generation) == j.generation) {
            SDL_FlushAudioStream(s->conv);
            play(s, j.generation);
        }
        SDL_ClearAudioStream(s->conv);
        SDL_free(l->text);
        SDL_free(l);
        SDL_LockMutex(s->lock);
        s->audio = (double)j.samples / POCKET_RATE;
        s->generating = false;
    }
    SDL_UnlockMutex(s->lock);
    return 0;
}

static void render(void *user, float *lr, int frames) {
    gear_say *s = user;
    int factor = s->rate / 16000;
    for (int done = 0; done < frames;) {
        int k = SDL_min(frames - done, SCRATCH);
        SDL_memset(s->scratch, 0, (size_t)k * 2 * sizeof(float));
        gs_stream_render(s->stream, s->scratch, k);
        for (int i = 0; i < k; i++) {
            lr[2 * (done + i)] += s->scratch[2 * i];
            lr[2 * (done + i) + 1] += s->scratch[2 * i + 1];
            if (!s->echo) continue;
            s->echo_sum += (s->scratch[2 * i] + s->scratch[2 * i + 1]) * 0.5f;
            if (++s->echo_count < factor) continue;
            s->echo_frame[s->echo_fill++] = s->echo_sum / (float)factor;
            s->echo_sum = 0;
            s->echo_count = 0;
            if (s->echo_fill == 160) {
                gs_voice_played(s->echo, s->echo_frame);
                s->echo_fill = 0;
            }
        }
        done += k;
    }
}

// Warnings and errors are kept; the engine's progress lines only with GEAR_DEBUG set.
static void on_log(enum pt_log_level level, const char *msg, void *user) {
    if (level >= PT_LOG_WARN || SDL_getenv("GEAR_DEBUG")) SDL_Log("pocket: %s", msg);
}

gear_say *gear_say_open(const char *model, const char *voice, int threads, gs_voice *echo, char *error, size_t error_size) {
    pt_log_set(on_log, NULL);
    int rate = gs_mix_rate();
    if (rate % 16000) {
        SDL_snprintf(error, error_size, "The mix runs at %d Hz; the echo canceller needs a multiple of 16000", rate);
        return NULL;
    }
    // hearsay's pockettts.cpp reads its CPU thread count from POCKET_THREADS.
    char n[16];
    SDL_snprintf(n, sizeof n, "%d", threads);
    SDL_setenv_unsafe("POCKET_THREADS", n, 1);
    struct pt_init_params ip;
    pt_init_default_params(&ip);
    ip.model_path = model;
    ip.device = "CPU";
    struct pt_context *ctx = pt_init(&ip);
    if (!ctx) {
        SDL_snprintf(error, error_size, "Could not load %s: %s", model, pt_last_error());
        return NULL;
    }
    struct pt_voice *v = pt_voice_load(ctx, voice, 0);
    if (!v) {
        SDL_snprintf(error, error_size, "Could not load the voice %s: %s", voice, pt_last_error());
        pt_free(ctx);
        return NULL;
    }
    gear_say *s = SDL_calloc(1, sizeof *s);
    s->ctx = ctx;
    s->voice = v;
    s->rate = rate;
    s->echo = echo;
    // Four seconds ahead at most: synthesis runs faster than speech and then waits on the player.
    s->stream = gs_stream_new(rate, 4.0, 0.05);
    const SDL_AudioSpec from = { SDL_AUDIO_F32, 1, POCKET_RATE }, to = { SDL_AUDIO_F32, 2, rate };
    s->conv = SDL_CreateAudioStream(&from, &to);
    s->lock = SDL_CreateMutex();
    s->wake = SDL_CreateCondition();
    s->thread = SDL_CreateThread(run, "gear-say", s);
    gs_mix_add(render, s);
    return s;
}

void gear_say_close(gear_say *s) {
    if (!s) return;
    gear_say_stop(s);
    SDL_LockMutex(s->lock);
    s->quit = true;
    SDL_SignalCondition(s->wake);
    SDL_UnlockMutex(s->lock);
    gs_stream_stop(s->stream);  // unblocks a write waiting for room
    SDL_WaitThread(s->thread, NULL);
    gs_mix_remove(render, s);
    gs_stream_free(s->stream);
    SDL_DestroyAudioStream(s->conv);
    pt_voice_free(s->voice);
    pt_free(s->ctx);
    SDL_DestroyCondition(s->wake);
    SDL_DestroyMutex(s->lock);
    SDL_free(s);
}

void gear_say_speak(gear_say *s, const char *text) {
    line *l = SDL_calloc(1, sizeof *l), **q;
    l->text = SDL_strdup(text);
    SDL_LockMutex(s->lock);
    for (q = &s->queue; *q; q = &(*q)->next) {}
    *q = l;
    SDL_SignalCondition(s->wake);
    SDL_UnlockMutex(s->lock);
}

void gear_say_stop(gear_say *s) {
    SDL_LockMutex(s->lock);
    SDL_AddAtomicInt(&s->generation, 1);
    for (line *l = s->queue, *next; l; l = next) {
        next = l->next;
        SDL_free(l->text);
        SDL_free(l);
    }
    s->queue = NULL;
    SDL_UnlockMutex(s->lock);
    gs_stream_flush(s->stream);
}

bool gear_say_busy(gear_say *s) {
    SDL_LockMutex(s->lock);
    bool busy = s->queue || s->generating;
    SDL_UnlockMutex(s->lock);
    return busy || gs_stream_buffered(s->stream) > 0;
}

void gear_say_last(gear_say *s, double *first_audio, double *audio) {
    SDL_LockMutex(s->lock);
    if (first_audio) *first_audio = s->first_audio;
    if (audio) *audio = s->audio;
    SDL_UnlockMutex(s->lock);
}
