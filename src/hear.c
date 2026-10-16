#include "hear.h"
#include <SDL3/SDL.h>
#include "transcribe.h"

typedef struct piece { struct piece *next; float *samples; int n; char *text; } piece;

struct gear_hear {
    struct transcribe_session *session;
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *wake;
    piece *todo, *done;  // queues, oldest first
    int pending;
    double last_seconds;
    bool quit;
};

static void push(piece **q, piece *p) {
    p->next = NULL;
    while (*q) q = &(*q)->next;
    *q = p;
}

static void free_list(piece *p) {
    for (piece *next; p; p = next) {
        next = p->next;
        SDL_free(p->samples);
        SDL_free(p->text);
        SDL_free(p);
    }
}

static int run(void *user) {
    gear_hear *h = user;
    struct transcribe_run_params rp;
    transcribe_run_params_init(&rp);
    rp.language = "en";
    SDL_LockMutex(h->lock);
    for (;;) {
        while (!h->todo && !h->quit) SDL_WaitCondition(h->wake, h->lock);
        if (h->quit) break;
        piece *p = h->todo;
        h->todo = p->next;
        SDL_UnlockMutex(h->lock);
        Uint64 t = SDL_GetTicksNS();
        transcribe_status st = transcribe_run(h->session, p->samples, p->n, &rp);
        double seconds = (double)(SDL_GetTicksNS() - t) / 1e9;
        const char *text = st == TRANSCRIBE_OK ? transcribe_full_text(h->session) : "";
        // Parakeet starts its text with a space; the pipeline joins pieces with one of its own.
        while (*text == ' ') text++;
        p->text = SDL_strdup(text);
        if (st != TRANSCRIBE_OK) SDL_Log("gear: transcription failed: %s", transcribe_status_string(st));
        SDL_free(p->samples);
        p->samples = NULL;
        SDL_LockMutex(h->lock);
        h->last_seconds = seconds;
        push(&h->done, p);
        h->pending--;
    }
    SDL_UnlockMutex(h->lock);
    return 0;
}

// Warnings and errors are kept; the engine's progress lines only with GEAR_DEBUG set.
static void on_log(transcribe_log_level level, const char *msg, void *user) {
    if (level == TRANSCRIBE_LOG_LEVEL_WARN || level == TRANSCRIBE_LOG_LEVEL_ERROR || SDL_getenv("GEAR_DEBUG")) SDL_Log("transcribe: %s", msg);
}

gear_hear *gear_hear_open(const char *model, int threads, char *error, size_t error_size) {
    transcribe_log_set(on_log, NULL);
    struct transcribe_session_params sp;
    transcribe_session_params_init(&sp);
    sp.n_threads = threads;
    struct transcribe_session *session = NULL;
    transcribe_status st = transcribe_open(model, NULL, &sp, &session);
    if (st != TRANSCRIBE_OK) {
        SDL_snprintf(error, error_size, "Could not load %s: %s", model, transcribe_status_string(st));
        return NULL;
    }
    gear_hear *h = SDL_calloc(1, sizeof *h);
    h->session = session;
    h->lock = SDL_CreateMutex();
    h->wake = SDL_CreateCondition();
    h->thread = SDL_CreateThread(run, "gear-hear", h);
    return h;
}

void gear_hear_close(gear_hear *h) {
    if (!h) return;
    SDL_LockMutex(h->lock);
    h->quit = true;
    SDL_SignalCondition(h->wake);
    SDL_UnlockMutex(h->lock);
    SDL_WaitThread(h->thread, NULL);
    free_list(h->todo);
    free_list(h->done);
    transcribe_close(h->session);
    SDL_DestroyCondition(h->wake);
    SDL_DestroyMutex(h->lock);
    SDL_free(h);
}

void gear_hear_submit(gear_hear *h, const float *samples, int n) {
    piece *p = SDL_calloc(1, sizeof *p);
    p->samples = SDL_malloc((size_t)n * sizeof(float));
    SDL_memcpy(p->samples, samples, (size_t)n * sizeof(float));
    p->n = n;
    SDL_LockMutex(h->lock);
    push(&h->todo, p);
    h->pending++;
    SDL_SignalCondition(h->wake);
    SDL_UnlockMutex(h->lock);
}

char *gear_hear_take(gear_hear *h) {
    SDL_LockMutex(h->lock);
    piece *p = h->done;
    if (p) h->done = p->next;
    SDL_UnlockMutex(h->lock);
    if (!p) return NULL;
    char *text = p->text;
    SDL_free(p);
    return text;
}

int gear_hear_pending(gear_hear *h) {
    SDL_LockMutex(h->lock);
    int n = h->pending;
    SDL_UnlockMutex(h->lock);
    return n;
}

double gear_hear_last_seconds(gear_hear *h) {
    SDL_LockMutex(h->lock);
    double s = h->last_seconds;
    SDL_UnlockMutex(h->lock);
    return s;
}
