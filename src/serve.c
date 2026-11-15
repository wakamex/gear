#define _POSIX_C_SOURCE 200809L  // (fdopen and dup, for keeping standard output to the frames)
#include "serve.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "agent.h"
#include "env.h"
#include "gs_json.h"
#include "hear.h"
#include "gs_voice.h"
#include "models.h"
#include "pipeline.h"
#include "pocket.h"
#include "relay.h"
#include "speakable.h"
#include "transcript.h"
#include "vad.h"

enum { READ_LIMIT = 1200, SPEECH_RATE = 24000, MAX_AUDIO = 60 * 16000 };

typedef struct frame { struct frame *next; char type; Uint32 len; char *data; } frame;

typedef struct {
    // Frames from the client, read on a thread of their own; `closed` once its input ends.
    SDL_Mutex *lock;
    frame *in;
    bool closed;
    // What goes out, one frame at a time, from the main thread and the synthesis thread.
    SDL_Mutex *out;
    // Speech: text queued for the synthesis thread, and the voice to load before the next.
    SDL_Condition *wake;
    char *speak, *next_voice;
    bool quit;
    SDL_AtomicInt generation;  // advanced by a stop; a synthesis of an older one gives up
    struct pt_context *pocket;
    struct pt_voice *voice;
    bool synthesizing;
    // The reply being spoken, at the microphone's 16 kHz, for the echo canceller and the check against
    // the speakers' words: what the phone plays, as far as its reports ('Q') say it has.
    SDL_AudioStream *to16;
    float *reply16;
    int reply16_len, reply16_cap;
    bool phone_playing;
    bool stopping;     // a stop was sent: reports of playing are old until the phone says it is quiet
    double phone_at;   // seconds of the reply played, as last reported
    long since_at;     // microphone samples since that report
} server;

static FILE *frames_out;  // the client's end; what the engines print goes to standard error instead

static void send_frame(server *s, char type, const void *data, Uint32 len) {
    Uint8 head[5] = { (Uint8)type, (Uint8)len, (Uint8)(len >> 8), (Uint8)(len >> 16), (Uint8)(len >> 24) };
    SDL_LockMutex(s->out);
    fwrite(head, 1, 5, frames_out);
    if (len) fwrite(data, 1, len, frames_out);
    fflush(frames_out);
    SDL_UnlockMutex(s->out);
}

static void send_text(server *s, char type, const char *text) { send_frame(s, type, text, (Uint32)SDL_strlen(text)); }

static int read_frames(void *user) {
    server *s = user;
    for (;;) {
        Uint8 head[5];
        if (fread(head, 1, 5, stdin) != 5) break;
        Uint32 len = (Uint32)head[1] | (Uint32)head[2] << 8 | (Uint32)head[3] << 16 | (Uint32)head[4] << 24;
        if (len > 8 * 1024 * 1024) break;  // (not a client of this protocol)
        frame *f = SDL_calloc(1, sizeof *f);
        f->type = (char)head[0], f->len = len, f->data = SDL_malloc(len + 1);
        if (len && fread(f->data, 1, len, stdin) != len) { SDL_free(f->data), SDL_free(f); break; }
        f->data[len] = 0;
        SDL_LockMutex(s->lock);
        frame **q = &s->in;
        while (*q) q = &(*q)->next;
        *q = f;
        SDL_UnlockMutex(s->lock);
    }
    SDL_LockMutex(s->lock);
    s->closed = true;
    SDL_UnlockMutex(s->lock);
    return 0;
}

typedef struct { server *s; int generation; } job;

static bool on_chunk(const float *samples, int n, void *user) {
    job *j = user;
    if (SDL_GetAtomicInt(&j->s->generation) != j->generation) return false;
    Sint16 *pcm = SDL_malloc((size_t)n * sizeof *pcm);
    for (int i = 0; i < n; i++) pcm[i] = (Sint16)SDL_clamp(samples[i] * 32767.0f, -32768.0f, 32767.0f);
    // (kept at 16 kHz too, as the reference for what the phone plays)
    SDL_LockMutex(j->s->lock);
    SDL_PutAudioStreamData(j->s->to16, samples, n * (int)sizeof(float));
    int got = SDL_GetAudioStreamAvailable(j->s->to16) / (int)sizeof(float);
    if (j->s->reply16_len + got > j->s->reply16_cap) {
        j->s->reply16_cap = (j->s->reply16_len + got) * 2;
        j->s->reply16 = SDL_realloc(j->s->reply16, (size_t)j->s->reply16_cap * sizeof(float));
    }
    j->s->reply16_len += SDL_GetAudioStreamData(j->s->to16, j->s->reply16 + j->s->reply16_len, got * (int)sizeof(float)) / (int)sizeof(float);
    SDL_UnlockMutex(j->s->lock);
    send_frame(j->s, 'P', pcm, (Uint32)n * 2);
    SDL_free(pcm);
    return SDL_GetAtomicInt(&j->s->generation) == j->generation;
}

// Speech, one reply at a time, as it is made: each chunk goes out as it comes.
static int synthesize(void *user) {
    server *s = user;
    SDL_LockMutex(s->lock);
    for (;;) {
        while (!s->speak && !s->next_voice && !s->quit) SDL_WaitCondition(s->wake, s->lock);
        if (s->quit) break;
        char *voice = s->next_voice, *text = s->speak;
        s->next_voice = NULL, s->speak = NULL;
        job j = { s, SDL_GetAtomicInt(&s->generation) };
        SDL_UnlockMutex(s->lock);
        if (voice) {
            struct pt_voice *v = pt_voice_load(s->pocket, voice, 0);
            if (v) pt_voice_free(s->voice), s->voice = v;
            SDL_free(voice);
        }
        if (text) {
            SDL_LockMutex(s->lock);
            s->synthesizing = true;
            SDL_UnlockMutex(s->lock);
            struct pt_tts_params p;
            pt_tts_default_params(&p);
            p.text = text;
            p.voice = s->voice;
            p.on_chunk = on_chunk;
            p.user_data = &j;
            pt_synthesize(s->pocket, &p, NULL);
            if (SDL_GetAtomicInt(&s->generation) == j.generation) send_frame(s, 'D', NULL, 0);
            SDL_free(text);
            SDL_LockMutex(s->lock);
            s->synthesizing = false;
            SDL_UnlockMutex(s->lock);
        }
        SDL_LockMutex(s->lock);
    }
    SDL_UnlockMutex(s->lock);
    return 0;
}

static void speak(server *s, const char *reply) {
    char *spoken = gear_speakable(reply, READ_LIMIT);
    SDL_LockMutex(s->lock);
    // A new reply: its reference starts empty, as the phone's count of what it has played does.
    s->reply16_len = 0, s->phone_at = 0, s->since_at = 0;
    SDL_ClearAudioStream(s->to16);
    SDL_free(s->speak);
    s->speak = spoken;
    SDL_SignalCondition(s->wake);
    SDL_UnlockMutex(s->lock);
}

// What the phone's speaker plays over the next GEAR_FRAME samples of its microphone, by its last report.
static void phone_played(server *s, float *out) {
    SDL_LockMutex(s->lock);
    long at = (long)(s->phone_at * GEAR_RATE) + s->since_at;
    for (int i = 0; i < GEAR_FRAME; i++) out[i] = s->phone_playing && at + i >= 0 && at + i < s->reply16_len ? s->reply16[at + i] : 0;
    s->since_at += GEAR_FRAME;
    SDL_UnlockMutex(s->lock);
}

// The phone as the player of replies, for the pipeline's rule that talking over a reply stops it.
static bool phone_busy(void *user) {
    server *s = user;
    SDL_LockMutex(s->lock);
    bool busy = s->synthesizing || s->speak || s->phone_playing;
    SDL_UnlockMutex(s->lock);
    return busy;
}

static void phone_stop(void *user) {
    server *s = user;
    SDL_LockMutex(s->lock);
    SDL_AddAtomicInt(&s->generation, 1);
    SDL_free(s->speak), s->speak = NULL;
    s->phone_playing = false, s->stopping = true;
    SDL_UnlockMutex(s->lock);
    send_frame(s, 'I', NULL, 0);
}

static void list_agents(server *s) {
    gear_agent *all = SDL_malloc(256 * sizeof *all);
    char error[512];
    int n = gear_agents_list(all, 256, error, sizeof error);
    if (n < 0) { send_text(s, 'F', error); SDL_free(all); return; }
    gear_agent_tabs(all, n);
    gs_jval *list = gs_jval_array();
    for (int i = 0; i < n; i++) {
        gs_jval *a = gs_jval_object();
        gs_jval_set(a, "name", gs_jval_str(all[i].name));
        gs_jval_set(a, "tab", gs_jval_str(all[i].tab));
        gs_jval_set(a, "harness", gs_jval_str(all[i].harness));
        gs_jval_set(a, "status", gs_jval_str(all[i].status));
        gs_jval_push(list, a);
    }
    char *json = gs_jval_write(list, false);
    send_text(s, 'G', json);
    SDL_free(json);
    gs_jval_free(list);
    SDL_free(all);
}

// What is being said in always-on mode, for the phone to show as it comes: the message so far, the
// piece being said, and the seconds before it is sent (-1 when not counting).
static void words(server *s, gear_pipeline *p) {
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "text", gs_jval_str(gear_pipeline_text(p)));
    gs_jval_set(o, "draft", gs_jval_str(gear_pipeline_draft(p)));
    gs_jval_set(o, "left", gs_jval_num(SDL_round(gear_pipeline_countdown(p) * 10) / 10));
    char *json = gs_jval_write(o, false);
    send_text(s, 'W', json);
    SDL_free(json);
    gs_jval_free(o);
}

static void hello(server *s, const char *target, const char *voice) {
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "target", gs_jval_str(target));
    gs_jval_set(o, "voice", gs_jval_str(voice));
    char *json = gs_jval_write(o, false);
    send_text(s, 'H', json);
    SDL_free(json);
    gs_jval_free(o);
}

// A message to the agent: shown to the phone, kept in the transcript, and sent, or read back when no
// agent is chosen.
typedef struct { gear_relay **relay; const char *target; gear_transcript *transcript; } delivery;
static delivery out_to;

static void deliver_text(server *s, const char *text) {
    char status[200];
    send_text(s, 'M', text);
    gear_transcript_add(out_to.transcript, text, out_to.target);
    if (*out_to.relay) {
        gear_relay_send(*out_to.relay, text);
        SDL_snprintf(status, sizeof status, "waiting for %s", out_to.target);
        send_text(s, 'S', status);
    } else send_text(s, 'R', text), speak(s, text);
}

int gear_serve(const char *models, int hear_threads, int say_threads) {
    char path[1200], name[128], error[512];
#ifndef _WIN32
    // The engines print progress to standard output in places: that goes to standard error, and the
    // frames to a copy of standard output kept for them.
    frames_out = fdopen(dup(1), "wb");
    dup2(2, 1);
#else
    frames_out = stdout;
#endif
    gear_hear_model_pick(models, NULL, path, sizeof path, name, sizeof name);
    gear_hear *hear = gear_hear_open(path, hear_threads, error, sizeof error);
    server s = { 0 };
    s.lock = SDL_CreateMutex(), s.out = SDL_CreateMutex(), s.wake = SDL_CreateCondition();
    const SDL_AudioSpec speech = { SDL_AUDIO_F32, 1, SPEECH_RATE }, mic = { SDL_AUDIO_F32, 1, GEAR_RATE };
    s.to16 = SDL_CreateAudioStream(&speech, &mic);
    if (!hear) { send_text(&s, 'F', error); return 1; }
    char threads[16];
    SDL_snprintf(threads, sizeof threads, "%d", say_threads);
    gear_setenv("POCKET_THREADS", threads);
    struct pt_init_params ip;
    pt_init_default_params(&ip);
    gear_model_path(models, GEAR_MODEL_SAY, path, sizeof path);
    ip.model_path = path;
    if (!(s.pocket = pt_init(&ip))) { SDL_snprintf(error, sizeof error, "Could not load %s: %s", path, pt_last_error()); send_text(&s, 'F', error); return 1; }
    gear_model voice = gear_voice_remembered(models);
    gear_model_path(models, voice, path, sizeof path);
    s.voice = pt_voice_load(s.pocket, path, 0);
    // The agent last sent to from here, as the window remembers it.
    char target[128] = "", host[128] = "";
    gear_target_remembered(models, target, sizeof target, host, sizeof host);
    gear_relay *relay = target[0] ? gear_relay_new(target) : NULL;
    char *pref = SDL_GetPrefPath("wakamex", "gear");
    SDL_snprintf(path, sizeof path, "%smessages.jsonl", pref ? pref : "");
    SDL_free(pref);
    gear_transcript *transcript = gear_transcript_open(path, 1);
    out_to = (delivery){ &relay, target, transcript };
    SDL_Thread *reader = SDL_CreateThread(read_frames, "gear-serve-in", &s);
    SDL_Thread *speaker = SDL_CreateThread(synthesize, "gear-serve-say", &s);
    hello(&s, target, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");

    float *audio = SDL_malloc(MAX_AUDIO * sizeof(float));
    int samples = 0;
    // Always-on: the phone's audio through gear's own listener, as the window's, which finds the
    // messages itself; opened the first time it is asked for.
    gs_voice *voice_dsp = NULL;
    gear_vad *vad = NULL;
    gear_pipeline *pipe = NULL;
    bool always = false;
    char last_words[2400] = "";
    float pending[160];
    int pending_n = 0;
    for (bool open = true; open;) {
        SDL_LockMutex(s.lock);
        frame *f = s.in;
        if (f) s.in = f->next;
        open = f || !s.closed;
        SDL_UnlockMutex(s.lock);
        if (f) {
            switch (f->type) {
            case 'A':
                for (Uint32 i = 0; i + 1 < f->len; i += 2) {
                    float x = (float)(Sint16)((Uint8)f->data[i] | (Uint8)f->data[i + 1] << 8) / 32768.0f;
                    if (!always) { if (samples < MAX_AUDIO) audio[samples++] = x; continue; }
                    pending[pending_n++] = x;
                    if (pending_n == GEAR_FRAME) {
                        // What the phone played meanwhile, for the echo canceller and the speakers' words.
                        float played[GEAR_FRAME];
                        phone_played(&s, played);
                        gs_voice_played(voice_dsp, played);
                        gear_pipeline_frame(pipe, pending, played);
                        pending_n = 0;
                    }
                }
                break;
            case 'O':  // always-on, "1" or "0"
                always = f->data[0] == '1';
                if (always && !pipe) {
                    gear_model_path(models, GEAR_MODEL_VAD, path, sizeof path);
                    if (!(vad = gear_vad_open(path, error, sizeof error))) { send_text(&s, 'F', error); always = false; break; }
                    // (with WebRTC's echo canceller where this build has it, given what the phone plays)
                    voice_dsp = gs_voice_new(GEAR_RATE, GS_VOICE_SPEECH | (gs_voice_has_echo() ? GS_VOICE_ECHO : 0));
                    gear_listen_params lp = gear_listen_defaults();
                    pipe = gear_pipeline_new(voice_dsp, vad, hear, NULL, &lp, gear_countdown_remembered(models, 3));
                    gear_pipeline_set_speaker(pipe, &(gear_speaker){ phone_busy, phone_stop, &s });
                    // Words as they are said, where the recogniser has a GPU (as the window's).
                    if (SDL_strcasecmp(gear_hear_device(hear), "cpu")) gear_pipeline_set_drafter(pipe, hear);
                }
                if (!always && pipe) gear_pipeline_cancel(pipe);
                pending_n = 0, samples = 0, last_words[0] = 0;
                break;
            case 'Q': {  // the phone's playback: "1 SECONDS" while it plays the reply, that far in; "0" when not
                SDL_LockMutex(s.lock);
                if (f->data[0] != '1') s.stopping = false;
                s.phone_playing = f->data[0] == '1' && !s.stopping;
                s.phone_at = f->len > 2 ? SDL_atof(f->data + 2) : 0;
                s.since_at = 0;
                SDL_UnlockMutex(s.lock);
                break;
            }
            case 'N':  // always-on: send what has been said now
                if (pipe) {
                    char *now = gear_pipeline_take(pipe);
                    if (now) deliver_text(&s, now), SDL_free(now);
                }
                break;
            case 'E': {
                if (always) break;
                if (samples < 16000 / 4) { send_text(&s, 'F', "too short to hear"); samples = 0; break; }
                send_text(&s, 'S', "hearing");
                // A little silence after, as a piece of speech has from its pause.
                for (int i = 0; i < 16000 / 4 && samples < MAX_AUDIO; i++) audio[samples++] = 0;
                gear_hear_submit(hear, audio, samples);
                samples = 0;
                char *text = NULL;
                while (!(text = gear_hear_take(hear, NULL))) SDL_Delay(5);
                if (!*text) send_text(&s, 'F', "heard no words");
                else deliver_text(&s, text);
                SDL_free(text);
                break;
            }
            case 'X':
                SDL_AddAtomicInt(&s.generation, 1);
                break;
            case 'L':
                list_agents(&s);
                break;
            case 'T': {
                gear_agent a;
                if (!gear_agent_find(f->data, &a, error, sizeof error)) { send_text(&s, 'F', error); break; }
                gear_relay_free(relay);  // (a reply the agent before still owed is given up)
                relay = gear_relay_new(a.name);
                SDL_strlcpy(target, a.name, sizeof target);
                gear_target_remember(models, a.name, "");
                hello(&s, target, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");
                break;
            }
            case 'V':
                voice = !SDL_strcmp(f->data, "mary") ? GEAR_MODEL_VOICE_MARY : GEAR_MODEL_VOICE_ALBA;
                gear_voice_remember(models, voice);
                gear_model_path(models, voice, path, sizeof path);
                SDL_LockMutex(s.lock);
                SDL_free(s.next_voice);
                s.next_voice = SDL_strdup(path);
                SDL_SignalCondition(s.wake);
                SDL_UnlockMutex(s.lock);
                hello(&s, target, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");
                break;
            }
            SDL_free(f->data);
            SDL_free(f);
            continue;
        }
        if (always && pipe) {
            char *message = gear_pipeline_poll(pipe);
            if (message) deliver_text(&s, message), SDL_free(message), last_words[0] = 0;
            char now[2400];
            SDL_snprintf(now, sizeof now, "%s|%s|%.1f", gear_pipeline_text(pipe), gear_pipeline_draft(pipe), gear_pipeline_countdown(pipe));
            if (SDL_strcmp(now, last_words)) SDL_strlcpy(last_words, now, sizeof last_words), words(&s, pipe);
        }
        bool failed;
        char *reply;
        while (relay && ((reply = gear_relay_take(relay, &failed, error, sizeof error)) || failed)) {
            if (failed) { send_text(&s, 'F', error); break; }
            send_text(&s, 'R', reply);
            speak(&s, reply);
            SDL_free(reply);
        }
        SDL_Delay(10);
    }
    SDL_LockMutex(s.lock);
    s.quit = true;
    SDL_AddAtomicInt(&s.generation, 1);
    SDL_SignalCondition(s.wake);
    SDL_UnlockMutex(s.lock);
    SDL_WaitThread(speaker, NULL);
    SDL_DetachThread(reader);  // (it is done, its input closed)
    gear_relay_free(relay);
    gear_transcript_close(transcript);
    gear_pipeline_free(pipe);
    gear_vad_free(vad);
    if (voice_dsp) gs_voice_free(voice_dsp);
    gear_hear_close(hear);
    pt_voice_free(s.voice);
    SDL_DestroyAudioStream(s.to16);
    SDL_free(s.reply16);
    pt_free(s.pocket);
    SDL_free(audio);
    return 0;
}
