#include "pipeline.h"
#include <SDL3/SDL.h>
#include "gs_json.h"
#include "wav.h"

enum { HOLD_FRAMES = 30 };  // 0.3 s of talking stops a reply
enum { PLAYED_KEPT = 32 * GEAR_RATE, LEAD = 3 * GEAR_RATE / 10 };  // a piece is 30 s at most; 0.3 s more before it
// A piece whose loudest 100 ms is quieter than this is not sent to the recognizer, which writes words
// into breathing and room noise. Of the 119 kept pieces it found words in, the 9 below it were "Yeah",
// "Okay", "So", "Thank you", "absolutely" (breathing) and the like; sentences of three words or more
// peaked at 0.0147 (median), 95% of them above 0.0041.
#define QUIET_PIECE 0.0015f

static bool too_quiet(const float *piece, int n) {
    enum { WINDOW = GEAR_RATE / 10 };  // 100 ms
    for (int at = 0; at + WINDOW <= n; at += WINDOW / 2) {
        float sum = 0;
        for (int i = at; i < at + WINDOW; i++) sum += piece[i] * piece[i];
        if (sum >= QUIET_PIECE * QUIET_PIECE * WINDOW) return false;
    }
    return true;
}

struct gear_pipeline {
    gs_voice *voice;
    gear_vad *vad;
    gear_hear *hear;
    gear_say *say;
    gear_listen *listen;
    double countdown;
    long frames;           // microphone frames so far
    long last_end;         // the frame the last piece ended on, or -1
    char *text;            // the message so far
    size_t len, cap;
    int interruptions;
    float on;              // the speech score that counts as talking
    int held;              // frames in a row of talking while a reply plays
    // What the speakers played, the last PLAYED_KEPT samples, beside the microphone's.
    float *played;
    long played_at;        // samples written so far
    // For each piece sent to the recogniser, oldest first, whether the speakers' sound over the same
    // stretch follows it there; and a piece's text waiting for that.
    bool referenced[64];
    int ref_head, ref_count;
    char *waiting;
    // Kept speech (gear_pipeline_keep): pieces saved as they end, their text written as it comes.
    char keep[1024], model[128];
    long message;            // the message the pieces belong to, counted from 1
    char pending[64][48];    // pieces saved and not yet transcribed, oldest first, relative to keep
    int pend_head, pend_count;
};

gear_pipeline *gear_pipeline_new(gs_voice *voice, gear_vad *vad, gear_hear *hear, gear_say *say, const gear_listen_params *listen, double countdown) {
    gear_pipeline *p = SDL_calloc(1, sizeof *p);
    p->voice = voice;
    p->vad = vad;
    p->hear = hear;
    p->say = say;
    p->listen = gear_listen_new(listen);
    p->on = listen->on;
    p->played = SDL_calloc(PLAYED_KEPT, sizeof(float));
    p->countdown = countdown;
    p->last_end = -1;
    p->cap = 256;
    p->text = SDL_calloc(1, p->cap);
    return p;
}

void gear_pipeline_free(gear_pipeline *p) {
    if (!p) return;
    gear_listen_free(p->listen);
    SDL_free(p->text);
    SDL_free(p->played);
    SDL_free(p->waiting);
    SDL_free(p);
}

void gear_pipeline_keep(gear_pipeline *p, const char *dir, const char *model) {
    SDL_strlcpy(p->keep, dir ? dir : "", sizeof p->keep);
    SDL_strlcpy(p->model, model ? model : "", sizeof p->model);
    if (p->keep[0] && !SDL_CreateDirectory(p->keep)) p->keep[0] = 0;
    p->message = 1;
}

void gear_pipeline_set_hear(gear_pipeline *p, gear_hear *hear, const char *model) {
    p->hear = hear;
    SDL_strlcpy(p->model, model ? model : "", sizeof p->model);
    p->pend_count = 0;  // pieces the old recogniser had not finished are not transcribed
    p->ref_count = 0;
    SDL_free(p->waiting), p->waiting = NULL;
}

static void keep_line(gear_pipeline *p, gs_jval *o) {
    char path[1100];
    SDL_snprintf(path, sizeof path, "%s/heard.jsonl", p->keep);
    char *line = gs_jval_write(o, false);
    SDL_IOStream *io = line ? SDL_IOFromFile(path, "ab") : NULL;
    if (io) {
        SDL_WriteIO(io, line, SDL_strlen(line));
        SDL_WriteIO(io, "\n", 1);
        SDL_CloseIO(io);
    }
    SDL_free(line);
    gs_jval_free(o);
}

// A piece saved as the recogniser gets it, in a folder for the day, named for the local time.
static void keep_piece(gear_pipeline *p, const float *piece, int n) {
    if (!p->keep[0] || p->pend_count == 64) return;
    SDL_Time now;
    SDL_DateTime t;
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &t, true)) return;
    char day[16], *name = p->pending[(p->pend_head + p->pend_count) % 64], path[1200];
    SDL_snprintf(day, sizeof day, "%04d-%02d-%02d", t.year, t.month, t.day);
    SDL_snprintf(name, 48, "%s/%02d%02d%02d-%03d.wav", day, t.hour, t.minute, t.second, t.nanosecond / 1000000);
    SDL_snprintf(path, sizeof path, "%s/%s", p->keep, day);
    SDL_CreateDirectory(path);
    SDL_snprintf(path, sizeof path, "%s/%s", p->keep, name);
    if (gear_wav_write(path, piece, n, 1, GEAR_RATE)) p->pend_count++;
}

static void keep_text(gear_pipeline *p, const char *text) {
    if (!p->keep[0] || !p->pend_count) return;
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "file", gs_jval_str(p->pending[p->pend_head]));
    gs_jval_set(o, "text", gs_jval_str(text));
    gs_jval_set(o, "model", gs_jval_str(p->model));
    gs_jval_set(o, "seconds", gs_jval_num(SDL_round(gear_hear_last_seconds(p->hear) * 1000) / 1000));
    gs_jval_set(o, "message", gs_jval_num((double)p->message));
    keep_line(p, o);
    p->pend_head = (p->pend_head + 1) % 64;
    p->pend_count--;
}

// What became of a message: "sent", "sent now" (Enter) or "dropped" (Esc), with its text.
static void keep_outcome(gear_pipeline *p, const char *outcome) {
    if (!p->keep[0] || !p->len) return;
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "message", gs_jval_num((double)p->message));
    gs_jval_set(o, "outcome", gs_jval_str(outcome));
    gs_jval_set(o, "text", gs_jval_str(p->text));
    keep_line(p, o);
    p->message++;
}

static void reset(gear_pipeline *p) {
    p->len = 0;
    p->text[0] = 0;
    p->last_end = -1;
}

static void drain(gear_pipeline *p);

void gear_pipeline_frame(gear_pipeline *p, float *frame, const float *played) {
    for (int i = 0; i < GEAR_FRAME; i++) p->played[(p->played_at + i) % PLAYED_KEPT] = played ? played[i] : 0;
    p->played_at += GEAR_FRAME;
    gs_voice_frame(p->voice, frame);
    p->frames++;
    float speech = gear_vad_frame(p->vad, frame);
    // A reply stops for talking held 0.3 s. Through loudspeakers a low vowel of the reply can reach the
    // microphone in a way the echo canceller does not model, a burst of 0.15 s that the voice detector
    // takes for speech; stopping at the first 50 ms of speech cut replies off a sentence in.
    p->held = p->say && gear_say_busy(p->say) && speech >= p->on ? p->held + 1 : 0;
    if (p->held == HOLD_FRAMES) {
        gear_say_stop(p->say);
        p->interruptions++;
    }
    switch (gear_listen_frame(p->listen, frame, speech)) {
    case GEAR_LISTEN_START:
        break;
    case GEAR_LISTEN_END: {
        int n;
        const float *piece = gear_listen_piece(p->listen, &n);
        if (too_quiet(piece, n)) {
            if (SDL_getenv("GEAR_DEBUG")) SDL_Log("gear: a piece of %.2f s too quiet to be speech, dropped", (double)n / GEAR_RATE);
            break;
        }
        gear_hear_submit(p->hear, piece, n);
        keep_piece(p, piece, n);
        // The speakers over the same stretch, from a little before it, when they played anything.
        int m = SDL_min(n + LEAD, PLAYED_KEPT);
        float *heard_out = SDL_malloc((size_t)m * sizeof(float));
        for (int i = 0; i < m; i++) heard_out[i] = p->played[((p->played_at - m + i) % PLAYED_KEPT + PLAYED_KEPT) % PLAYED_KEPT];
        bool refer = p->ref_count < 64 && !too_quiet(heard_out, m);
        if (refer) gear_hear_submit(p->hear, heard_out, m);
        SDL_free(heard_out);
        if (p->ref_count < 64) p->referenced[(p->ref_head + p->ref_count++) % 64] = refer;
        p->last_end = p->frames;
        break;
    }
    case GEAR_LISTEN_NONE: break;
    }
}

static void append(gear_pipeline *p, const char *s) {
    size_t n = SDL_strlen(s);
    if (!n) return;
    size_t need = p->len + n + 2;
    if (need > p->cap) {
        while (p->cap < need) p->cap *= 2;
        p->text = SDL_realloc(p->text, p->cap);
    }
    if (p->len) p->text[p->len++] = ' ';
    SDL_memcpy(p->text + p->len, s, n + 1);
    p->len += n;
}

// The countdown runs once the user has stopped and every piece has its text.
static bool counting(const gear_pipeline *p) {
    return p->len && p->last_end >= 0 && !gear_listen_in_piece(p->listen) && !gear_hear_pending(p->hear);
}

static double left(const gear_pipeline *p) { return p->countdown - (double)(p->frames - p->last_end) / 100.0; }

void gear_pipeline_set_countdown(gear_pipeline *p, double seconds) { p->countdown = seconds; }

double gear_pipeline_countdown(const gear_pipeline *p) { return counting(p) ? SDL_max(left(p), 0) : -1; }

static void drain(gear_pipeline *p) {
    for (;;) {
        if (!p->waiting && !(p->waiting = gear_hear_take(p->hear))) return;
        bool refer = p->ref_count && p->referenced[p->ref_head];
        char *played = refer ? gear_hear_take(p->hear) : NULL;
        if (refer && !played) return;  // (the speakers' text is still being made)
        if (p->ref_count) p->ref_head = (p->ref_head + 1) % 64, p->ref_count--;
        char *t = p->waiting;
        p->waiting = NULL;
        bool echo = p->say && gear_say_echoes(p->say, t);
        bool played_back = played && gear_mostly_in(t, played, 1);
        if (SDL_getenv("GEAR_DEBUG"))
            SDL_Log("gear: heard \"%s\"%s%s%s%s", t, played ? ", the speakers \"" : "", played ? played : "", played ? "\"" : "",
                    echo ? ": the reply's echo, dropped" : played_back ? ": the speakers' sound, dropped" : "");
        keep_text(p, t);
        if (!echo && !played_back) append(p, t);
        SDL_free(t);
        SDL_free(played);
    }
}

char *gear_pipeline_poll(gear_pipeline *p) {
    drain(p);
    if (!counting(p) || left(p) > 0) return NULL;
    char *message = SDL_strdup(p->text);
    keep_outcome(p, "sent");
    reset(p);
    return message;
}

char *gear_pipeline_take(gear_pipeline *p) {
    drain(p);
    if (!p->len) return NULL;
    char *message = SDL_strdup(p->text);
    keep_outcome(p, "sent now");
    reset(p);
    return message;
}

bool gear_pipeline_pending(const gear_pipeline *p) { return gear_hear_pending(p->hear) > 0; }

const char *gear_pipeline_text(const gear_pipeline *p) { return p->text; }
bool gear_pipeline_hearing(const gear_pipeline *p) { return gear_listen_in_piece(p->listen); }
double gear_pipeline_time(const gear_pipeline *p) { return (double)p->frames / 100.0; }
int gear_pipeline_interruptions(const gear_pipeline *p) { return p->interruptions; }

void gear_pipeline_cancel(gear_pipeline *p) {
    drain(p);
    keep_outcome(p, "dropped");
    reset(p);
}
