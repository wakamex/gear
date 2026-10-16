#include "pipeline.h"
#include <SDL3/SDL.h>

struct gear_pipeline {
    gs_voice *voice;
    gear_hear *hear;
    gear_say *say;
    gear_listen *listen;
    double countdown;
    long frames;           // microphone frames so far
    long last_end;         // the frame the last piece ended on, or -1
    char *text;            // the message so far
    size_t len, cap;
    int interruptions;
};

gear_pipeline *gear_pipeline_new(gs_voice *voice, gear_hear *hear, gear_say *say, const gear_listen_params *listen, double countdown) {
    gear_pipeline *p = SDL_calloc(1, sizeof *p);
    p->voice = voice;
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

void gear_pipeline_frame(gear_pipeline *p, float *frame) {
    gs_voice_frame(p->voice, frame);
    p->frames++;
    switch (gear_listen_frame(p->listen, frame, gs_voice_speech(p->voice))) {
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

char *gear_pipeline_poll(gear_pipeline *p) {
    for (char *t; (t = gear_hear_take(p->hear));) {
        append(p, t);
        SDL_free(t);
    }
    if (!counting(p) || left(p) > 0) return NULL;
    char *message = SDL_strdup(p->text);
    gear_pipeline_cancel(p);
    return message;
}

const char *gear_pipeline_text(const gear_pipeline *p) { return p->text; }
bool gear_pipeline_hearing(const gear_pipeline *p) { return gear_listen_in_piece(p->listen); }
double gear_pipeline_time(const gear_pipeline *p) { return (double)p->frames / 100.0; }
int gear_pipeline_interruptions(const gear_pipeline *p) { return p->interruptions; }

void gear_pipeline_cancel(gear_pipeline *p) {
    p->len = 0;
    p->text[0] = 0;
    p->last_end = -1;
}
