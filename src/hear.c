#include "hear.h"
#include <SDL3/SDL.h>
#include "transcribe.h"

typedef struct piece { struct piece *next; float *samples; int n; char *text; gear_hear_scores scores; } piece;

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
        // The recognizer's own scores for the piece, as means over the word pieces it wrote (hear.h). A
        // model that gives none leaves the count at 0.
        gear_hear_scores sum = { 0 };
        for (int i = 0, tokens = st == TRANSCRIBE_OK ? transcribe_n_tokens(h->session) : 0; i < tokens; i++) {
            struct transcribe_token tok;
            transcribe_token_init(&tok);
            if (transcribe_get_token(h->session, i, &tok) != TRANSCRIBE_OK || SDL_isnan(tok.logit)) continue;
            sum.tokens++;
            sum.raw += tok.logit, sum.blank += tok.logit - tok.blank_logit, sum.second += tok.logit - tok.runner_up_logit;
            sum.energy += tok.logsumexp, sum.pmax += SDL_expf(tok.logit - tok.logsumexp), sum.conf += tok.p;
        }
        if (sum.tokens) {
            float n = (float)sum.tokens;
            sum.raw /= n, sum.blank /= n, sum.second /= n, sum.energy /= n, sum.pmax /= n, sum.conf /= n;
        }
        p->scores = sum;
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

char *gear_hear_take(gear_hear *h, gear_hear_scores *scores) {
    SDL_LockMutex(h->lock);
    piece *p = h->done;
    if (p) h->done = p->next;
    SDL_UnlockMutex(h->lock);
    if (!p) return NULL;
    if (scores) *scores = p->scores;
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

const char *gear_hear_device(gear_hear *h) { return transcribe_model_backend(transcribe_get_model(h->session)); }
