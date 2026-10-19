#include "window.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include "agent.h"
#include "gs_draw.h"
#include "gs_mix.h"
#include "gs_text.h"
#include "gs_ui.h"
#include "gs_update.h"
#include "gs_voice.h"
#include "hear.h"
#include "listen.h"
#include "models.h"
#include "pipeline.h"
#include "relay.h"
#include "say.h"
#include "speakable.h"
#include "wav.h"

enum { MIX_RATE = 48000, READ_LIMIT = 1200, LEVELS = 6, KEPT = 4 * GEAR_RATE };  // KEPT: microphone audio kept for a report

// toolcore-native's palette.
static SDL_FColor hex(Uint32 c) { return (SDL_FColor){ (float)(c >> 16 & 255) / 255, (float)(c >> 8 & 255) / 255, (float)(c & 255) / 255, 1 }; }
#define BACKGROUND hex(0x040506)
#define PANEL hex(0x0a0908)
#define RAISED hex(0x121110)
#define BORDER hex(0x3d3428)
#define TEXT hex(0xb6aa99)
#define BRIGHT hex(0xe0e0e0)
#define DIM hex(0x7a6e5e)
#define ACCENT hex(0xc4862c)
#define OK hex(0x6b8f5e)
#define OVER hex(0xb84a3a)
#define INPUT hex(0x060504)

// Everything the window shows in one frame, from the live loop or a made-up exchange.
typedef struct {
    const char *state;      // LISTENING, HEARING, SENDING, WAITING, SPEAKING, MUTED
    SDL_FColor state_color;
    char target[200];
    float levels[LEVELS];   // the microphone's recent levels, 0 to 1, oldest first
    bool speech;            // the voice detector hears speech now
    const char *message;
    bool pending;           // speech heard and not yet in the message
    double left, total;     // the countdown, left < 0 when not counting
    const char *reply;
    bool speaking;
    char status[300];
    bool fault;
} view;

// ---- Drawing ----

static float px, small;  // the text size and the small size, in points
static gs_rect status_line;  // where the status bar's first line was drawn, for a click on it

// A panel: a frame with its title set into the top rule. Returns the inside.
static gs_rect panel(gs_ui *ui, gs_rect r, const char *title) {
    gs_ui_fill(ui, r, PANEL);
    gs_ui_frame(ui, r, BORDER);
    if (title) {
        float w = gs_ui_text_width(ui, title, small);
        gs_rect t = { r.x + 10, r.y - small * 0.7f, w + 8, small * 1.4f };
        gs_ui_fill(ui, t, BACKGROUND);
        gs_ui_text(ui, (gs_rect){ t.x + 4, t.y, w, t.h }, title, small, DIM, -1);
    }
    return gs_inset(r, 10);
}

static void state_line(gs_ui *ui, gs_rect r, const view *v) {
    gs_rect dot = gs_cut_left(&r, 16);
    gs_ui_circle(ui, dot.x + 5, dot.y + dot.h / 2, 4.5f, v->state_color);
    gs_rect word = gs_cut_left(&r, gs_ui_text_width(ui, v->state, small) + 18);
    gs_ui_text(ui, word, v->state, small, v->state_color, -1);
    // The microphone's meter, at the right: dim bars that turn to text colour while speech is heard.
    gs_rect meter = gs_cut_right(&r, LEVELS * 5 + 34);
    gs_rect label = gs_cut_right(&meter, 30);
    gs_ui_text(ui, label, "mic", small, DIM, 1);
    for (int i = 0; i < LEVELS; i++) {
        float h = 2 + 12 * SDL_clamp(v->levels[i], 0, 1);
        gs_ui_fill(ui, (gs_rect){ meter.x + (float)i * 5, meter.y + meter.h / 2 + 7 - h, 3, h }, v->speech ? TEXT : DIM);
    }
    gs_ui_text(ui, r, v->target, small, TEXT, -1);
}

static float countdown_height(void) { return small * 1.6f; }

static void countdown(gs_ui *ui, gs_rect r, const view *v) {
    int cells = 10, filled = (int)SDL_ceil(v->left / v->total * cells);
    for (int i = 0; i < cells; i++) {
        gs_rect c = { r.x + (float)i * 11, r.y + r.h / 2 - 4, 9, 8 };
        gs_ui_fill(ui, c, i < filled ? ACCENT : INPUT);
        if (i >= filled) gs_ui_frame(ui, c, BORDER);
    }
    char hint[128];
    SDL_snprintf(hint, sizeof hint, "sending in %.0f s   enter sends · esc cancels", SDL_ceil(v->left));
    gs_ui_text(ui, (gs_rect){ r.x + cells * 11 + 10, r.y, r.w - cells * 11 - 10, r.h }, hint, small, DIM, -1);
}

static float message_height(gs_ui *ui, const view *v, float w) {
    const char *t = *v->message ? v->message : "say something";
    return SDL_max(gs_ui_text_height(ui, t, px, w), gs_ui_line_height(ui, px) * 2) + countdown_height() + 4;
}

static void message(gs_ui *ui, gs_rect r, const view *v) {
    char text[4200];
    SDL_snprintf(text, sizeof text, "%s%s", v->message, v->pending ? (*v->message ? " …" : "…") : "");
    if (*text) gs_ui_text_wrapped(ui, r, text, px, TEXT);
    else gs_ui_text_wrapped(ui, r, "say something", px, DIM);
    if (v->left >= 0) countdown(ui, (gs_rect){ r.x, r.y + r.h - countdown_height(), r.w, countdown_height() }, v);
}

// A box for what the reading skipped: code and tables stay visible, titled as not read.
static float skipped(gs_ui *ui, gs_rect r, const char *title, const char *lines, int count, bool draw) {
    int shown = SDL_min(count, 6);
    float lh = gs_ui_line_height(ui, small), top = lh / 2 + 4, h = 6 + top + 6 + lh * (float)(shown + (count > shown));
    if (!draw) return h;
    gs_rect box = { r.x, r.y + 6, r.w, h - 6 };
    gs_ui_fill(ui, box, RAISED);
    gs_ui_frame(ui, box, BORDER);
    float w = gs_ui_text_width(ui, title, small);
    gs_ui_fill(ui, (gs_rect){ box.x + 8, box.y - lh / 2, w + 8, lh }, RAISED);
    gs_ui_text(ui, (gs_rect){ box.x + 12, box.y - lh / 2, w, lh }, title, small, DIM, -1);
    const char *l = lines;
    for (int i = 0; i < shown; i++) {
        const char *end = SDL_strchr(l, '\n');
        size_t n = end ? (size_t)(end - l) : SDL_strlen(l);
        char line[300];
        SDL_strlcpy(line, l, SDL_min(n + 1, sizeof line));
        gs_ui_text(ui, (gs_rect){ box.x + 10, box.y + top + lh * (float)i, box.w - 20, lh }, line, small, DIM, -1);
        l = end ? end + 1 : l + n;
    }
    if (count > shown) gs_ui_text(ui, (gs_rect){ box.x + 10, box.y + top + lh * (float)shown, box.w - 20, lh }, "…", small, DIM, -1);
    return h;
}

// The reply as written: paragraphs wrapped, fenced code and tables in boxes. Returns its height.
typedef struct { gs_ui *ui; gs_rect r; float y; SDL_FColor color; bool draw; } flow;

static void paragraph(flow *f, char *text, size_t *len) {
    if (!*len) return;
    text[*len - 1] = 0;  // the last line's break
    gs_rect at = { f->r.x, f->y, f->r.w, 100000 };
    f->y += (f->draw ? gs_ui_text_wrapped(f->ui, at, text, px, f->color) : gs_ui_text_height(f->ui, text, px, f->r.w)) + 6;
    *len = 0;
}

static void add_line(char *buf, size_t *len, const char *line) {
    size_t n = SDL_strlen(line);
    SDL_memcpy(buf + *len, line, n);
    *len += n;
    buf[(*len)++] = '\n';
    buf[*len] = 0;
}

static float reply(gs_ui *ui, gs_rect r, const char *text, SDL_FColor color, bool draw) {
    flow f = { ui, r, r.y, color, draw };
    char *copy = SDL_strdup(text ? text : "");
    size_t size = SDL_strlen(copy) + 2, plen = 0, blen = 0;
    char *para = SDL_calloc(1, size), *block = SDL_calloc(1, size);
    int lines = 0;
    bool in_code = false;
    for (char *l = copy, *next; l; l = next) {
        next = SDL_strchr(l, '\n');
        if (next) *next++ = 0;
        const char *t = l;
        while (*t == ' ') t++;
        bool fence = !SDL_strncmp(t, "```", 3) || !SDL_strncmp(t, "~~~", 3);
        if (in_code) {
            if (fence) {
                f.y += skipped(ui, (gs_rect){ r.x, f.y, r.w, 0 }, "code, not read", block, lines, draw) + 4;
                in_code = false, blen = 0, lines = 0;
            } else add_line(block, &blen, l), lines++;
            continue;
        }
        if (fence) { paragraph(&f, para, &plen); in_code = true; continue; }
        if (*t == '|') {
            paragraph(&f, para, &plen);
            add_line(block, &blen, l), lines++;
            // The table ends at the first line that is not a row of it.
            const char *n = next ? next : "";
            while (*n == ' ') n++;
            if (*n != '|') {
                f.y += skipped(ui, (gs_rect){ r.x, f.y, r.w, 0 }, "table, not read", block, lines, draw) + 4;
                blen = 0, lines = 0;
            }
            continue;
        }
        if (!*t) { paragraph(&f, para, &plen); continue; }
        add_line(para, &plen, l);
    }
    if (in_code && lines) f.y += skipped(ui, (gs_rect){ r.x, f.y, r.w, 0 }, "code, not read", block, lines, draw);
    paragraph(&f, para, &plen);
    SDL_free(copy);
    SDL_free(para);
    SDL_free(block);
    return f.y - r.y;
}

static void draw(gs_ui *ui, const view *v) {
    gs_rect all = gs_ui_begin(ui);
    gs_ui_fill(ui, all, BACKGROUND);
    gs_rect r = gs_inset(all, 10);
    state_line(ui, gs_cut_top(&r, 22), v);
    gs_cut_top(&r, 14);
    // The status bar at the bottom: facts a line each (the models, then the rest), or a fault in rust.
    const char *second = SDL_strchr(v->status, '\n');
    float line = small * 1.5f;
    gs_rect bar = gs_cut_bottom(&r, line * (second ? 2 : 1) + 4);
    gs_ui_fill(ui, (gs_rect){ bar.x, bar.y, bar.w, 1 }, BORDER);
    char first[300];
    SDL_strlcpy(first, v->status, second ? SDL_min((size_t)(second - v->status) + 1, sizeof first) : sizeof first);
    status_line = (gs_rect){ bar.x, bar.y + 4, bar.w, line };
    gs_ui_text(ui, status_line, first, small, v->fault ? OVER : DIM, -1);
    if (second) gs_ui_text(ui, (gs_rect){ bar.x, bar.y + 4 + line, bar.w, line }, second + 1, small, DIM, -1);
    gs_cut_bottom(&r, 8);
    float mh = message_height(ui, v, r.w - 20) + 20;
    gs_rect m = gs_cut_top(&r, mh);
    message(ui, panel(ui, m, "MESSAGE"), v);
    gs_cut_top(&r, 16);
    gs_rect inside = panel(ui, r, "REPLY");
    gs_ui_clip(ui, &inside);
    if (v->reply && *v->reply) {
        // A long reply shows its end, where the reading is going.
        float h = reply(ui, inside, v->reply, v->speaking ? BRIGHT : TEXT, false);
        gs_rect at = inside;
        if (h > inside.h) at.y -= h - inside.h;
        reply(ui, at, v->reply, v->speaking ? BRIGHT : TEXT, true);
    } else {
        gs_ui_text_wrapped(ui, inside, "the agent's reply is read here", px, DIM);
    }
    gs_ui_clip(ui, NULL);
    gs_ui_end(ui);
}

// ---- Updates ----

// Where published builds are, unless GEAR_UPDATE_FROM names another manifest (for trying it out).
#define UPDATES "http://updates.waka.mx/gear/manifest.txt"
// The publisher's key; tools/publish-update signs with its private half.
static const Uint8 publisher[32] = {
    0x45, 0xbb, 0xa1, 0x28, 0xcb, 0x43, 0x4d, 0xe9, 0xe0, 0x8d, 0x0a, 0xc5, 0x99, 0x0a, 0x27, 0x6b,
    0x9d, 0x6c, 0xf0, 0x33, 0x08, 0xde, 0x1c, 0xad, 0x1b, 0x07, 0x9a, 0xa5, 0xcf, 0xde, 0x20, 0x7a,
};

// A published build (named by -Dbuild) keeps itself up to date with gesso's gs_update; a build of
// one's own never looks.
static gs_update *start_updates(void) {
#ifdef GEAR_BUILD
    const char *from = SDL_getenv("GEAR_UPDATE_FROM");
    gs_update_config c = { .build = GEAR_BUILD, .manifest = from && *from ? from : UPDATES, .key = publisher, .program = GEAR_PROGRAM, .agent = "gear" };
    return gs_update_start(&c);
#else
    return NULL;
#endif
}

// ---- Fetching models ----

// Missing model files download on a thread of their own while the window shows how far it has got,
// and the loop opens once all are in place: a first install, or a newer build that uses a new model.
typedef struct {
    const char *dir;
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_AtomicInt stop, done;
    long long received, total;
    double eta;
    bool ok;
    char error[256];
} fetcher;

static void fetch_progress(void *user, long long received, long long total, double eta) {
    fetcher *f = user;
    SDL_LockMutex(f->lock);
    f->received = received, f->total = total, f->eta = eta;
    SDL_UnlockMutex(f->lock);
}

static int fetch_run(void *user) {
    fetcher *f = user;
    char error[256] = "";
    bool ok = gear_models_fetch(f->dir, &f->stop, fetch_progress, f, error, sizeof error);
    SDL_LockMutex(f->lock);
    f->ok = ok;
    SDL_strlcpy(f->error, error, sizeof f->error);
    SDL_UnlockMutex(f->lock);
    SDL_SetAtomicInt(&f->done, 1);
    return 0;
}

static void fetch_status(fetcher *f, char *out, size_t size) {
    SDL_LockMutex(f->lock);
    if (f->total <= 0) SDL_strlcpy(out, "Downloading the models", size);
    else if (f->eta >= 0) SDL_snprintf(out, size, "Downloading the models: %.0f%% of %.0f MB, %.0f s left", 100.0 * (double)f->received / (double)f->total, (double)f->total / 1e6, f->eta);
    else SDL_snprintf(out, size, "Downloading the models: %.0f%% of %.0f MB", 100.0 * (double)f->received / (double)f->total, (double)f->total / 1e6);
    SDL_UnlockMutex(f->lock);
}

// ---- The live loop ----

typedef struct {
    const gear_window_options *o;
    gs_voice *voice;
    gear_vad *vad;
    gear_hear *hear;
    gear_say *say;
    gear_relay *relay;
    gear_pipeline *pipe;
    SDL_AudioStream *mic;
    bool muted;
    char *reply;
    float levels[LEVELS];
    float level_sum;
    int level_frames;
    char fault[256];
    char target[200];
    // The last KEPT samples heard, as recorded and as cleaned, for a report of an interruption.
    float *raw, *clean;
    int kept_at;
    Uint64 speaking_since;  // when the current reading started to play, or 0
    char hear_slug[96], say_slug[96], voice_slug[64];  // the model files in use, without their extensions
    char hear_file[128];
    int hear_at, hear_count;  // its place among the recognition models in the folder, from 1
} app;

static void fill(app *a, view *v) {
    SDL_zerop(v);
    bool speaking = a->say && gear_say_busy(a->say);
    double left = gear_pipeline_countdown(a->pipe);
    bool hearing = gear_pipeline_hearing(a->pipe), pending = gear_pipeline_pending(a->pipe);
    bool waiting = a->relay && gear_relay_waiting(a->relay);
    if (a->muted) v->state = "MUTED", v->state_color = OVER;
    else if (speaking) v->state = "SPEAKING", v->state_color = ACCENT;
    else if (left >= 0) v->state = "SENDING", v->state_color = ACCENT;
    else if (hearing || pending) v->state = "HEARING", v->state_color = ACCENT;
    else if (waiting) v->state = "WAITING", v->state_color = ACCENT;
    else v->state = "LISTENING", v->state_color = OK;
    SDL_strlcpy(v->target, a->target, sizeof v->target);
    SDL_memcpy(v->levels, a->levels, sizeof v->levels);
    v->speech = hearing;
    v->message = gear_pipeline_text(a->pipe);
    v->pending = hearing || pending;
    v->left = left;
    v->total = a->o->countdown;
    v->reply = a->reply;
    v->speaking = speaking;
    if (a->fault[0]) {
        SDL_strlcpy(v->status, a->fault, sizeof v->status);
        v->fault = true;
    } else {
        bool echo = !a->o->no_echo && gs_voice_has_echo();
        char which[48] = "";
        if (a->hear_count > 1) SDL_snprintf(which, sizeof which, " (%d of %d, click to switch)", a->hear_at, a->hear_count);
        const char *device = gear_hear_device(a->hear);
        SDL_snprintf(v->status, sizeof v->status, "hear %s%s · say %s, %s\n%s · %s", a->hear_slug, which, a->say_slug, a->voice_slug,
                     SDL_strcasecmp(device, "cpu") ? "on the GPU" : "on the CPU", echo ? "echo cancel on" : "headphones");
    }
}

static void demo(view *v) {
    SDL_zerop(v);
    v->state = "SENDING";
    v->state_color = ACCENT;
    SDL_strlcpy(v->target, "→ pockettts (claude)", sizeof v->target);
    float levels[LEVELS] = { 0.2f, 0.5f, 0.8f, 0.4f, 0.1f, 0.05f };
    SDL_memcpy(v->levels, levels, sizeof levels);
    v->message = "Fix the countdown so a long pause doesn't send the message, and then run the tests.";
    v->left = 1.6;
    v->total = 3;
    v->reply = "I found the bug. The countdown timer resets whenever the voice detector reports silence, so a long pause never sends the message. I moved the reset into the speech start handler in `src/listen.c`:\n\n```c\nif (l->below < l->p.pause_frames && l->piece_frames < l->p.max_frames)\n    return GEAR_LISTEN_NONE;\n```\n\nAll 14 tests pass. Want me to commit it?";
    SDL_strlcpy(v->status, "hear parakeet-ft-v1-Q4_K_M · say pocket-tts-english_2026-09-Q8_0, alba\non the GPU · echo cancel on", sizeof v->status);
}

// A reading stopped by talking: logged, and with GEAR_DEBUG the last seconds the microphone heard are
// written beside the log, as recorded and as cleaned, to tell the user's voice from the reply's echo.
static void write_kept(const float *ring, int at, const char *path) {
    float *line = SDL_malloc(KEPT * sizeof(float));
    SDL_memcpy(line, ring + at, (size_t)(KEPT - at) * sizeof(float));
    SDL_memcpy(line + (KEPT - at), ring, (size_t)at * sizeof(float));
    gear_wav_write(path, line, KEPT, 1, GEAR_RATE);
    SDL_free(line);
}

static void report_interruption(app *a) {
    double into = a->speaking_since ? (double)(SDL_GetTicks() - a->speaking_since) / 1000 : -1;
    printf("[the reading stopped %.1f s in: talking heard, speech %.2f, echo cancel %s]\n", into, gs_voice_speech(a->voice), !a->o->no_echo && gs_voice_has_echo() ? "on" : "off");
    fflush(stdout);
    if (!SDL_getenv("GEAR_DEBUG")) return;
    char path[1200];
    Uint64 t = SDL_GetTicks();
    SDL_snprintf(path, sizeof path, "%sinterrupted-%llu-raw.wav", SDL_GetBasePath(), (unsigned long long)t);
    write_kept(a->raw, a->kept_at, path);
    SDL_snprintf(path, sizeof path, "%sinterrupted-%llu-clean.wav", SDL_GetBasePath(), (unsigned long long)t);
    write_kept(a->clean, a->kept_at, path);
}

// The microphone's frames into the pipeline, and its level for the meter.
static void listen_frames(app *a) {
    float frame[GEAR_FRAME];
    while (SDL_GetAudioStreamAvailable(a->mic) >= (int)sizeof frame) {
        SDL_GetAudioStreamData(a->mic, frame, sizeof frame);
        if (a->muted) continue;
        float sum = 0;
        for (int i = 0; i < GEAR_FRAME; i++) sum += frame[i] * frame[i];
        a->level_sum += sum / GEAR_FRAME;
        // A bar of the meter every 50 ms, on a scale where -50 dB is empty and -10 dB full.
        if (++a->level_frames == 5) {
            float db = 10 * SDL_log10f(a->level_sum / 5 + 1e-10f);
            SDL_memmove(a->levels, a->levels + 1, sizeof(float) * (LEVELS - 1));
            a->levels[LEVELS - 1] = SDL_clamp((db + 50) / 40, 0, 1);
            a->level_sum = 0;
            a->level_frames = 0;
        }
        SDL_memcpy(a->raw + a->kept_at, frame, sizeof frame);
        int before = gear_pipeline_interruptions(a->pipe);
        gear_pipeline_frame(a->pipe, frame);
        SDL_memcpy(a->clean + a->kept_at, frame, sizeof frame);
        a->kept_at = (a->kept_at + GEAR_FRAME) % KEPT;
        if (gear_pipeline_interruptions(a->pipe) > before) report_interruption(a);
    }
}

static void deliver(app *a, char *message) {
    printf("> %s\n", message);
    fflush(stdout);
    if (a->relay) gear_relay_send(a->relay, message);
    else {  // no agent: the message is the reply, read back
        SDL_free(a->reply);
        a->reply = SDL_strdup(message);
        gear_say_speak(a->say, message);
    }
    SDL_free(message);
}

static void take_replies(app *a) {
    char error[512];
    bool failed;
    for (char *r; a->relay && ((r = gear_relay_take(a->relay, &failed, error, sizeof error)) || failed);) {
        if (failed) { SDL_strlcpy(a->fault, error, sizeof a->fault); break; }
        a->fault[0] = 0;
        printf("%s\n", r);
        fflush(stdout);
        SDL_free(a->reply);
        a->reply = r;
        char *spoken = gear_speakable(r, READ_LIMIT);
        gear_say_speak(a->say, spoken);
        SDL_free(spoken);
    }
}

// A model file's name without its extension: parakeet-tdt_ctc-110m-Q4_K_M.gguf reads parakeet-tdt_ctc-110m-Q4_K_M.
static void slug(char *out, size_t size, const char *file) {
    SDL_strlcpy(out, file, size);
    char *dot = SDL_strrchr(out, '.');
    if (dot) *dot = 0;
}

static void count_hear(app *a);

static bool open_loop(app *a, char *error, size_t error_size) {
    const gear_window_options *o = a->o;
    char path[1200], voice[1200];
    if (!gear_models_present(o->models)) { SDL_snprintf(error, error_size, "The models are not in %s; run gear fetch", o->models); return false; }
    bool echo = !o->no_echo && gs_voice_has_echo();
    a->voice = gs_voice_new(GEAR_RATE, GS_VOICE_SPEECH | (echo ? GS_VOICE_ECHO : 0));
    gear_model_path(o->models, GEAR_MODEL_VAD, path, sizeof path);
    if (!(a->vad = gear_vad_open(path, error, error_size))) return false;
    char hear_name[128];
    gear_hear_model_pick(o->models, o->hear_model, path, sizeof path, hear_name, sizeof hear_name);
    if (!(a->hear = gear_hear_open(path, o->hear_threads, error, error_size))) return false;
    slug(a->hear_slug, sizeof a->hear_slug, hear_name);
    SDL_strlcpy(a->hear_file, hear_name, sizeof a->hear_file);
    count_hear(a);
    if (!gs_mix_open(MIX_RATE)) { SDL_snprintf(error, error_size, "No playback device: %s", SDL_GetError()); return false; }
    gear_model_path(o->models, GEAR_MODEL_SAY, path, sizeof path);
    gear_model_path(o->models, GEAR_MODEL_VOICE, voice, sizeof voice);
    if (!(a->say = gear_say_open(path, voice, o->say_threads, echo ? a->voice : NULL, error, error_size))) return false;
    if (o->to) {
        gear_agent agent;
        if (!gear_agent_find(o->to, &agent, error, error_size)) return false;
        a->relay = gear_relay_new(agent.name);
        SDL_snprintf(a->target, sizeof a->target, "→ %s (%s)", agent.name, agent.harness);
    } else SDL_strlcpy(a->target, "→ nobody: each message is read back", sizeof a->target);
    const SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, GEAR_RATE };
    if (!(a->mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL))) { SDL_snprintf(error, error_size, "No microphone: %s", SDL_GetError()); return false; }
    SDL_ResumeAudioStreamDevice(a->mic);
    a->raw = SDL_calloc(KEPT, sizeof(float));
    a->clean = SDL_calloc(KEPT, sizeof(float));
    // The loop runs only once everything it needs is open: without it the window shows STOPPED.
    slug(a->say_slug, sizeof a->say_slug, gear_model_info(GEAR_MODEL_SAY)->name);
    slug(a->voice_slug, sizeof a->voice_slug, gear_model_info(GEAR_MODEL_VOICE)->name);
    gear_listen_params lp = gear_listen_defaults();
    a->pipe = gear_pipeline_new(a->voice, a->vad, a->hear, a->say, &lp, o->countdown);
    if (!o->no_keep) {
        char heard[1100], *pref = SDL_GetPrefPath("wakamex", "gear");
        SDL_snprintf(heard, sizeof heard, "%sheard", pref ? pref : "");
        SDL_free(pref);
        gear_pipeline_keep(a->pipe, heard, a->hear_file);
    }
    return true;
}

static void count_hear(app *a) {
    char names[16][128];
    a->hear_count = gear_hear_models(a->o->models, names, 16);
    a->hear_at = 0;
    for (int i = 0; i < a->hear_count; i++) if (!SDL_strcmp(names[i], a->hear_file)) a->hear_at = i + 1;
}

// The next recognition model in the models folder, loaded in place of the one in use and remembered.
static void switch_hear(app *a) {
    char names[16][128], path[1200], error[512];
    int n = gear_hear_models(a->o->models, names, 16), at = 0;
    if (n < 2) return;
    for (int i = 0; i < n; i++) if (!SDL_strcmp(names[i], a->hear_file)) at = i;
    const char *next = names[(at + 1) % n];
    SDL_snprintf(path, sizeof path, "%s/%s", a->o->models, next);
    gear_hear *h = gear_hear_open(path, a->o->hear_threads, error, sizeof error);
    if (!h) { SDL_strlcpy(a->fault, error, sizeof a->fault); return; }
    gear_hear *old = a->hear;
    a->hear = h;
    gear_pipeline_set_hear(a->pipe, h, next);
    gear_hear_close(old);
    SDL_strlcpy(a->hear_file, next, sizeof a->hear_file);
    slug(a->hear_slug, sizeof a->hear_slug, next);
    gear_hear_model_remember(a->o->models, next);
    count_hear(a);
    a->fault[0] = 0;
}

static void close_loop(app *a) {
    if (a->mic) SDL_DestroyAudioStream(a->mic);
    gear_pipeline_free(a->pipe);
    gear_vad_free(a->vad);
    gear_relay_free(a->relay);
    gear_say_close(a->say);
    if (a->say) gs_mix_close();
    gear_hear_close(a->hear);
    if (a->voice) gs_voice_free(a->voice);
    SDL_free(a->reply);
    SDL_free(a->raw);
    SDL_free(a->clean);
}

int gear_window(const gear_window_options *o) {
    if (o->shot) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen"), SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    // gear sits beside the work it is for: shown, it does not take the keyboard from the app in use.
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    // The window's icon on Windows: the one built into the program (assets/icon/gear.rc).
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, "wakamex");  // gear's data folder is wakamex/gear
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) { fprintf(stderr, "gear: %s\n", SDL_GetError()); return 1; }
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "gear");
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 560);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 480);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (o->shot ? 0 : SDL_WINDOW_HIDDEN));
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, o->shot != NULL);
    SDL_Window *win = SDL_CreateWindowWithProperties(p);
    // gesso puts the window back where it was, from window.txt in gear's data folder; a picture keeps the fixed size.
    if (win && o->shot) SDL_SetStringProperty(SDL_GetWindowProperties(win), GS_UI_PLACE, "");
    SDL_DestroyProperties(p);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!ren) { fprintf(stderr, "gear: no window: %s\n", SDL_GetError()); return 1; }
    // One monospace font, shipped beside the program, before the system's for what it lacks.
    gs_fontset *fonts = gs_fontset_system();
    char font[1100];
    SDL_snprintf(font, sizeof font, "%sClankerMono-Regular.ttf", SDL_GetBasePath());
    gs_fontset_prepend(fonts, font);
    gs_ui *ui = gs_ui_new(win, ren, fonts);
    if (!o->shot) SDL_ShowWindow(win);  // after gs_ui_new has placed it, so it does not jump
    gs_ui_style *st = gs_ui_style_of(ui);
    st->background = BACKGROUND, st->panel = PANEL, st->raised = RAISED, st->text = TEXT, st->muted = DIM;
    st->accent = ACCENT, st->border = BORDER, st->warning = OVER, st->radius = 0;
    px = 15;
    app a = { .o = o };
    view v;
    char error[512] = "";
    fetcher fetch = { .dir = o->models };
    bool fetching = !o->demo && !o->shot && o->models[0] && !gear_models_present(o->models);
    if (fetching) {
        fetch.lock = SDL_CreateMutex();
        fetch.thread = SDL_CreateThread(fetch_run, "gear-fetch", &fetch);
    } else if (!o->demo && !open_loop(&a, error, sizeof error)) SDL_strlcpy(a.fault, error, sizeof a.fault);
    int code = 0, frames = 0;
    Uint64 drawn = 0, busy = 0;
    // A published build keeps itself up to date, and starts the new one after 10 s of nothing to
    // hear, send, wait for or read, so that no conversation is cut off.
    gs_update *update = o->shot || o->demo ? NULL : start_updates();
    for (bool running = true; running;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 10)) {
            do {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
                gs_ui_event(ui, &e);
            } while (SDL_PollEvent(&e));
        }
        if (a.pipe) {
            listen_frames(&a);
            char *message = gear_pipeline_poll(a.pipe);
            if (message) deliver(&a, message);
            take_replies(&a);
            bool reading = gear_say_busy(a.say);
            if (reading && !a.speaking_since) a.speaking_since = SDL_GetTicks();
            if (!reading) a.speaking_since = 0;
            if (reading || gear_pipeline_countdown(a.pipe) >= 0 || gear_pipeline_hearing(a.pipe) || gear_pipeline_pending(a.pipe) ||
                *gear_pipeline_text(a.pipe) || (a.relay && gear_relay_waiting(a.relay)))
                busy = SDL_GetTicks();
        }
        // (not while muted: the new build would start listening)
        if (gs_update_ready(update) && !a.muted && SDL_GetTicks() - busy > 10000) {
            code = GEAR_UPDATED;
            running = false;
        }
        // Drawn twenty times a second: the meter and countdown move by themselves.
        if (SDL_GetTicks() - drawn < 50 && !o->shot) continue;
        drawn = SDL_GetTicks();
        small = SDL_roundf(px * 0.8f);
        st->text_px = px, st->small_px = small;
        if (fetching && SDL_GetAtomicInt(&fetch.done)) {
            SDL_WaitThread(fetch.thread, NULL);
            fetching = false;
            if (!fetch.ok) SDL_snprintf(a.fault, sizeof a.fault, "Could not download the models: %s", fetch.error);
            else if (!open_loop(&a, error, sizeof error)) SDL_strlcpy(a.fault, error, sizeof a.fault);
        }
        if (o->demo || !a.pipe) {
            if (o->demo) demo(&v);
            else if (fetching) {
                SDL_zero(v);
                v.state = "FETCHING", v.state_color = ACCENT, v.message = "", v.left = -1;
                fetch_status(&fetch, v.status, sizeof v.status);
            } else {
                SDL_zero(v);
                v.state = "STOPPED", v.state_color = OVER, v.message = "", v.left = -1, v.fault = true;
                SDL_strlcpy(v.status, a.fault, sizeof v.status);
            }
        } else fill(&a, &v);
        SDL_SetRenderDrawColorFloat(ren, BACKGROUND.r, BACKGROUND.g, BACKGROUND.b, 1);
        SDL_RenderClear(ren);
        draw(ui, &v);
        if (a.pipe) {  // the keys, after the frame has seen them
            if (gs_ui_key(ui, SDLK_SPACE, 0)) {
                a.muted = !a.muted;
                a.muted ? SDL_PauseAudioStreamDevice(a.mic) : SDL_ResumeAudioStreamDevice(a.mic);
            }
            if (gs_ui_key(ui, SDLK_ESCAPE, 0)) {
                if (gear_say_busy(a.say)) gear_say_stop(a.say);
                else gear_pipeline_cancel(a.pipe);
            }
            if (gs_ui_clicked(ui, status_line)) switch_hear(&a);
            if (gs_ui_key(ui, SDLK_RETURN, 0)) {
                char *message = gear_pipeline_take(a.pipe);
                if (message) deliver(&a, message);
            }
        }
        if (gs_ui_key(ui, SDLK_EQUALS, 0) || gs_ui_key(ui, SDLK_PLUS, 0) || gs_ui_key(ui, SDLK_KP_PLUS, 0)) px = SDL_min(px + 1, 28);
        if (gs_ui_key(ui, SDLK_MINUS, 0) || gs_ui_key(ui, SDLK_KP_MINUS, 0)) px = SDL_max(px - 1, 10);
        if (o->shot && ++frames >= 3 && gs_ui_wait_ms(ui) != 0) {
            code = gs_draw_save_png(ren, o->shot) ? 0 : 1;
            running = false;
        }
        SDL_RenderPresent(ren);
    }
    gs_update_free(update);
    if (fetching) {
        SDL_SetAtomicInt(&fetch.stop, 1);
        SDL_WaitThread(fetch.thread, NULL);
    }
    SDL_DestroyMutex(fetch.lock);
    close_loop(&a);
    gs_ui_free(ui);
    gs_fontset_free(fonts);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return code;
}
