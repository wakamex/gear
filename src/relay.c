#include "relay.h"
#include <SDL3/SDL.h>

typedef struct item { struct item *next; char *text; bool failed; } item;  // a message, or a reply

struct gear_relay {
    gear_agent agent;
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *wake;
    item *outbox, *inbox;
    int waiting;
    bool quit;
};

static void push(item **q, item *it) {
    it->next = NULL;
    while (*q) q = &(*q)->next;
    *q = it;
}

static void deliver(gear_relay *r, char *text, bool failed) {
    item *it = SDL_calloc(1, sizeof *it);
    it->text = text;
    it->failed = failed;
    SDL_LockMutex(r->lock);
    push(&r->inbox, it);
    r->waiting--;
    SDL_UnlockMutex(r->lock);
}

static bool quitting(gear_relay *r) {
    SDL_LockMutex(r->lock);
    bool q = r->quit;
    SDL_UnlockMutex(r->lock);
    return q;
}

static int run(void *user) {
    gear_relay *r = user;
    for (;;) {
        SDL_LockMutex(r->lock);
        while (!r->outbox && !r->quit) SDL_WaitCondition(r->wake, r->lock);
        if (r->quit) { SDL_UnlockMutex(r->lock); return 0; }
        item *m = r->outbox;
        r->outbox = m->next;
        SDL_UnlockMutex(r->lock);
        char id[37], error[512];
        bool sent = gear_agent_send(&r->agent, m->text, id, error, sizeof error);
        SDL_free(m->text);
        SDL_free(m);
        if (!sent) { deliver(r, SDL_strdup(error), true); continue; }
        // The agent works for seconds to minutes; asking twice a second costs a short process each time.
        for (;;) {
            if (quitting(r)) return 0;
            char *reply = NULL;
            gear_reply_state s = gear_agent_reply(id, &reply, error, sizeof error);
            if (s == GEAR_REPLY_DONE) { deliver(r, reply, false); break; }
            if (s == GEAR_REPLY_FAILED) { deliver(r, SDL_strdup(error), true); break; }
            SDL_Delay(500);
        }
    }
}

gear_relay *gear_relay_new(const gear_agent *agent) {
    gear_relay *r = SDL_calloc(1, sizeof *r);
    r->agent = *agent;
    r->lock = SDL_CreateMutex();
    r->wake = SDL_CreateCondition();
    r->thread = SDL_CreateThread(run, "gear-relay", r);
    return r;
}

static void free_items(item *it) {
    for (item *next; it; it = next) {
        next = it->next;
        SDL_free(it->text);
        SDL_free(it);
    }
}

void gear_relay_free(gear_relay *r) {
    if (!r) return;
    SDL_LockMutex(r->lock);
    r->quit = true;
    SDL_SignalCondition(r->wake);
    SDL_UnlockMutex(r->lock);
    SDL_WaitThread(r->thread, NULL);
    free_items(r->outbox);
    free_items(r->inbox);
    SDL_DestroyCondition(r->wake);
    SDL_DestroyMutex(r->lock);
    SDL_free(r);
}

void gear_relay_send(gear_relay *r, const char *text) {
    item *it = SDL_calloc(1, sizeof *it);
    it->text = SDL_strdup(text);
    SDL_LockMutex(r->lock);
    push(&r->outbox, it);
    r->waiting++;
    SDL_SignalCondition(r->wake);
    SDL_UnlockMutex(r->lock);
}

char *gear_relay_take(gear_relay *r, bool *failed, char *error, size_t error_size) {
    *failed = false;
    SDL_LockMutex(r->lock);
    item *it = r->inbox;
    if (it) r->inbox = it->next;
    SDL_UnlockMutex(r->lock);
    if (!it) return NULL;
    char *text = it->text;
    if (it->failed) {
        *failed = true;
        SDL_strlcpy(error, text, error_size);
        SDL_free(text);
        text = NULL;
    }
    SDL_free(it);
    return text;
}

bool gear_relay_waiting(gear_relay *r) {
    SDL_LockMutex(r->lock);
    bool w = r->waiting > 0;
    SDL_UnlockMutex(r->lock);
    return w;
}
