#include "pipeline.h"
#include <SDL3/SDL.h>
#include "gs_json.h"
#include "wav.h"

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

void gear_pipeline_frame(gear_pipeline *p, float *frame) {
    gs_voice_frame(p->voice, frame);
    p->frames++;
    switch (gear_listen_frame(p->listen, frame, gear_vad_frame(p->vad, frame))) {
    case GEAR_LISTEN_START:
        if (p->say && gear_say_busy(p->say)) {
            gear_say_stop(p->say);
            p->interruptions++;
        }
        break;
    case GEAR_LISTEN_END: {
        int n;
        const float *piece = gear_listen_piece(p->listen, &n);
        gear_hear_submit(p->hear, piece, n);
        keep_piece(p, piece, n);
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

double gear_pipeline_countdown(const gear_pipeline *p) { return counting(p) ? SDL_max(left(p), 0) : -1; }

static void drain(gear_pipeline *p) {
    for (char *t; (t = gear_hear_take(p->hear));) {
        if (SDL_getenv("GEAR_DEBUG")) SDL_Log("gear: heard \"%s\" in %.2f s", t, gear_hear_last_seconds(p->hear));
        keep_text(p, t);
        append(p, t);
        SDL_free(t);
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
