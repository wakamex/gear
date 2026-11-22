#include "panetone.h"
#include <SDL3/SDL.h>
#include <string.h>
#include "agent.h"
#include "gs_http.h"
#include "gs_json.h"

static const char *panetone(void) {
    const char *p = SDL_getenv("GEAR_PANETONE");
    return p && *p ? p : "panetone";
}

bool gear_panetone_say(const char *route, const char *text, const char *via, char *error, size_t error_size) {
    // Here the text goes on standard input, so it is not in the process list; over SSH it is the last
    // argument, as the Windows ssh client never passes on the end of piped input.
    bool remote = gear_agent_host() != NULL;
    const char *argv[] = { panetone(), "say", "--to", route, "--as", via, remote ? text : NULL, NULL };
    size_t len = 0;
    int code = -1;
    char *out = gear_host_run(argv, remote ? NULL : text, &len, &code);
    if (!out) { SDL_snprintf(error, error_size, "could not run panetone: %s", SDL_GetError()); return false; }
    // {"ok": true, "result": {"request_id", "accepted", "already_sent"}}, or {"ok": false, "error":
    // {"code", "message"}} and a line saying it was rejected
    gs_json *doc = gs_json_parse(out, strcspn(out, "\n"));
    gs_jv r = doc ? gs_json_root(doc) : NULL;
    bool ok = r && gs_json_bool(gs_json_get(r, "ok"), false) && gs_json_bool(gs_json_get(gs_json_get(r, "result"), "accepted"), false);
    if (!ok) {
        const char *why = r ? gs_json_str(gs_json_get(gs_json_get(r, "error"), "message"), NULL) : NULL;
        size_t n = strcspn(out, "\n");
        if (why) SDL_snprintf(error, error_size, "Panetone did not take the message for %s: %s", route, why);
        else SDL_snprintf(error, error_size, "Panetone did not take the message for %s: %.*s", route, (int)SDL_min(n, 300), n ? out : "no answer");
    }
    gs_json_free(doc);
    SDL_free(out);
    return ok;
}

bool gear_panetone_answer(const char *route, const char *action, const char *via, char *label, size_t label_size, char *error, size_t error_size) {
    const char *argv[] = { panetone(), "answer", "--to", route, "--action", action, "--as", via, NULL };
    size_t len = 0;
    int code = -1;
    char *out = gear_host_run(argv, NULL, &len, &code);
    if (!out) { SDL_snprintf(error, error_size, "could not run panetone: %s", SDL_GetError()); return false; }
    // {"ok": true, "result": {"answered": LABEL}}, or {"ok": false, "error": {"code", "message"}}
    // with code question_closed, unknown_action or answer_failed
    gs_json *doc = gs_json_parse(out, strcspn(out, "\n"));
    gs_jv r = doc ? gs_json_root(doc) : NULL;
    bool ok = r && gs_json_bool(gs_json_get(r, "ok"), false);
    if (ok) SDL_strlcpy(label, gs_json_str(gs_json_get(gs_json_get(r, "result"), "answered"), ""), label_size);
    else {
        const char *why = r ? gs_json_str(gs_json_get(gs_json_get(r, "error"), "code"), NULL) : NULL;
        if (why && !SDL_strcmp(why, "question_closed")) SDL_snprintf(error, error_size, "%s's question was already answered or closed", route);
        else if (why && !SDL_strcmp(why, "unknown_action")) SDL_snprintf(error, error_size, "%s no longer offers that answer", route);
        else {
            const char *m = r ? gs_json_str(gs_json_get(gs_json_get(r, "error"), "message"), NULL) : NULL;
            size_t n = strcspn(out, "\n");
            SDL_snprintf(error, error_size, "the answer did not reach %s: %.*s", route, (int)(m ? SDL_strlen(m) : SDL_min(n, 300)), m ? m : n ? out : "no answer");
        }
    }
    gs_json_free(doc);
    SDL_free(out);
    return ok;
}

// ---- The feed ----

// A line of the feed: kind is Panetone's ("agent", "question", "request", "notice", "owner",
// "turn_end"); id its message's; via, for an owner's message, the client it came through; final_id,
// for a turn's end, the id of the message that answers what began the turn.
typedef struct line {
    struct line *next;
    char *text;
    char who[128], at[8], kind[16], via[32], id[64], final_id[64];
    char actions[1536];  // its answers to press: "ID\tLABEL\n" each
    long long seq;
} line;

struct gear_feed {
    char seq_path[1100], via[32];
    SDL_Thread *thread;
    SDL_AtomicInt quit;
    SDL_Mutex *lock;
    line *in;            // come and not yet handed out, oldest first
    line *held;          // of the route talked to, waiting for its turn's end
    Uint64 held_at;
    long long seq_seen;  // the newest line handled
    long long seq_read;  // the newest line read, where `panetone watch` starts again when it ends
    char last_id[64];    // the last post of the route talked to given as a message, for a late turn end
    char *last_text;
};

static line *parse(const char *json, size_t n) {
    gs_json *doc = gs_json_parse(json, n);
    if (!doc) return NULL;
    gs_jv o = gs_json_root(doc);
    line *p = SDL_calloc(1, sizeof *p);
    p->seq = (long long)gs_json_num(gs_json_get(o, "seq"), 0);
    SDL_strlcpy(p->who, gs_json_str(gs_json_get(o, "route"), "panetone"), sizeof p->who);
    SDL_strlcpy(p->kind, gs_json_str(gs_json_get(o, "kind"), ""), sizeof p->kind);
    SDL_strlcpy(p->via, gs_json_str(gs_json_get(o, "via"), ""), sizeof p->via);
    SDL_strlcpy(p->id, gs_json_str(gs_json_get(o, "id"), ""), sizeof p->id);
    SDL_strlcpy(p->final_id, gs_json_str(gs_json_get(o, "final_id"), ""), sizeof p->final_id);
    for (gs_jv a = gs_json_first(gs_json_get(o, "actions")); a; a = gs_json_next(a)) {
        char one[600];
        SDL_snprintf(one, sizeof one, "%s\t%s\n", gs_json_str(gs_json_get(a, "id"), ""), gs_json_str(gs_json_get(a, "label"), ""));
        if (!SDL_strchr(gs_json_str(gs_json_get(a, "id"), ""), '\t') && SDL_strlen(p->actions) + SDL_strlen(one) < sizeof p->actions)
            SDL_strlcat(p->actions, one, sizeof p->actions);
    }
    // The text, and each attachment by name (the file stays on the computer).
    const char *text = gs_json_str(gs_json_get(o, "text"), "");
    size_t cap = SDL_strlen(text) + 1;
    for (gs_jv f = gs_json_first(gs_json_get(o, "attachments")); f; f = gs_json_next(f)) cap += SDL_strlen(gs_json_str(gs_json_get(f, "name"), "")) + 20;
    p->text = SDL_malloc(cap);
    SDL_strlcpy(p->text, text, cap);
    for (gs_jv f = gs_json_first(gs_json_get(o, "attachments")); f; f = gs_json_next(f))
        SDL_strlcat(p->text, *p->text ? "\n[attached: " : "[attached: ", cap), SDL_strlcat(p->text, gs_json_str(gs_json_get(f, "name"), "a file"), cap), SDL_strlcat(p->text, "]", cap);
    SDL_Time when = (SDL_Time)gs_json_num(gs_json_get(o, "time_ms"), 0) * 1000000;
    SDL_DateTime d;
    if (SDL_TimeToDateTime(when, &d, true)) SDL_snprintf(p->at, sizeof p->at, "%02d:%02d", d.hour, d.minute);
    gs_json_free(doc);
    return p;
}

static void free_line(line *p) {
    if (p) SDL_free(p->text), SDL_free(p);
}

// `panetone watch` from the position kept, here or over SSH, its lines queued as they come; started
// again 5 s after it ends.
static int follow(void *user) {
    gear_feed *f = user;
    while (!SDL_GetAtomicInt(&f->quit)) {
        // (from the position kept, the first time; from the last line read, after that)
        char *last = f->seq_read || !*f->seq_path ? NULL : SDL_LoadFile(f->seq_path, NULL), after[32] = "";
        if (last) SDL_snprintf(after, sizeof after, "%lld", SDL_strtoll(last, NULL, 10));
        else if (f->seq_read) SDL_snprintf(after, sizeof after, "%lld", f->seq_read);
        SDL_free(last);
        const char *host = gear_agent_host();
        char remote[256];
        SDL_snprintf(remote, sizeof remote, "%s watch%s%s", panetone(), *after ? " --after " : "", after);
        const char *here[] = { panetone(), "watch", *after ? "--after" : NULL, after, NULL };
        const char *there[] = { "ssh", "-T", host, remote, NULL };
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)(host ? there : here));
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_Process *p = gs_http_spawn(props);
        SDL_DestroyProperties(props);
        if (p) {
            SDL_IOStream *out = SDL_GetProcessOutput(p);
            size_t cap = 1 << 16, n = 0;
            char *buf = SDL_malloc(cap);
            while (!SDL_GetAtomicInt(&f->quit)) {
                if (n + 4096 > cap) buf = SDL_realloc(buf, cap *= 2);
                size_t got = SDL_ReadIO(out, buf + n, cap - n);
                if (!got) {
                    SDL_IOStatus st = SDL_GetIOStatus(out);
                    if (st != SDL_IO_STATUS_NOT_READY && st != SDL_IO_STATUS_READY) break;  // ended
                    SDL_Delay(50);
                    continue;
                }
                n += got;
                for (char *nl; (nl = memchr(buf, '\n', n));) {
                    line *l = parse(buf, (size_t)(nl - buf));
                    size_t used = (size_t)(nl - buf) + 1;
                    SDL_memmove(buf, buf + used, n - used), n -= used;
                    if (!l) continue;
                    SDL_LockMutex(f->lock);
                    if (l->seq > f->seq_read) f->seq_read = l->seq;
                    line **q = &f->in;
                    while (*q) q = &(*q)->next;
                    *q = l;
                    SDL_UnlockMutex(f->lock);
                }
            }
            SDL_free(buf);
            SDL_KillProcess(p, true);
            SDL_WaitProcess(p, true, NULL);
            SDL_DestroyProcess(p);
        }
        for (int i = 0; i < 50 && !SDL_GetAtomicInt(&f->quit); i++) SDL_Delay(100);
    }
    return 0;
}

gear_feed *gear_feed_open(const char *seq_path, const char *via) {
    gear_feed *f = SDL_calloc(1, sizeof *f);
    SDL_strlcpy(f->seq_path, seq_path, sizeof f->seq_path);
    SDL_strlcpy(f->via, via, sizeof f->via);
    f->lock = SDL_CreateMutex();
    f->thread = SDL_CreateThread(follow, "gear-feed", f);
    return f;
}

void gear_feed_close(gear_feed *f) {
    if (!f) return;
    SDL_SetAtomicInt(&f->quit, 1);
    SDL_WaitThread(f->thread, NULL);
    for (line *l = f->in, *next; l; l = next) next = l->next, free_line(l);
    free_line(f->held);
    SDL_free(f->last_text);
    SDL_DestroyMutex(f->lock);
    SDL_free(f);
}

// `text` handed out as a post from the line `l`'s route and time.
static void post_of(const line *l, gear_post_kind kind, bool quiet, bool shown, char *text, gear_post *out) {
    *out = (gear_post){ kind, quiet, shown, "", "", text, "" };
    SDL_strlcpy(out->actions, l->actions, sizeof out->actions);
    SDL_strlcpy(out->who, l->who, sizeof out->who), SDL_strlcpy(out->at, l->at, sizeof out->at);
}

// The held post handed out as a message, kept as the last for a turn's end that comes late.
static void give_held(gear_feed *f, gear_post *out) {
    SDL_strlcpy(f->last_id, f->held->id, sizeof f->last_id);
    SDL_free(f->last_text), f->last_text = SDL_strdup(f->held->text);
    post_of(f->held, GEAR_POST_MESSAGE, false, false, f->held->text, out);
    f->held->text = NULL;
    free_line(f->held), f->held = NULL;
}

static void save_position(gear_feed *f) {
    if (f->held || !f->seq_seen || !*f->seq_path) return;  // (a held post is read again after a restart)
    char seq[32];
    SDL_snprintf(seq, sizeof seq, "%lld", f->seq_seen);
    SDL_SaveFile(f->seq_path, seq, SDL_strlen(seq));
}

bool gear_feed_next(gear_feed *f, const char *route, gear_post *out) {
    for (;;) {
        // A held post whose turn's end has not come in 3 s, or whose route is no longer talked to.
        if (f->held && (SDL_GetTicks() - f->held_at > 3000 || SDL_strcmp(f->held->who, route))) {
            give_held(f, out);
            save_position(f);
            return true;
        }
        SDL_LockMutex(f->lock);
        line *l = f->in;
        if (l) f->in = l->next;
        SDL_UnlockMutex(f->lock);
        if (!l) return false;
        if (l->seq > f->seq_seen) f->seq_seen = l->seq;
        bool given = false, quiet = !SDL_strcmp(l->kind, "request") || !SDL_strcmp(l->kind, "notice");
        bool talked_to = *route && !SDL_strcmp(l->who, route);
        if (!SDL_strcmp(l->kind, "turn_end")) {
            // The reply: the held post the turn's end names, or one given already as a message.
            if (f->held && *l->final_id && !SDL_strcmp(f->held->id, l->final_id)) {
                post_of(f->held, GEAR_POST_REPLY, false, false, f->held->text, out);
                f->held->text = NULL;
                free_line(f->held), f->held = NULL, given = true;
            } else if (talked_to && *l->final_id && !SDL_strcmp(f->last_id, l->final_id) && f->last_text) {
                post_of(l, GEAR_POST_REPLY, false, true, SDL_strdup(f->last_text), out);
                f->last_id[0] = 0, given = true;
            }
        } else if (!SDL_strcmp(l->kind, "owner")) {
            // The user's own message, Panetone's copy: one this client sent is its own already; one
            // through another client is the user's, without its "[via X] ".
            if (SDL_strcmp(l->via, f->via)) {
                const char *text = l->text;
                if (*text == '[' && SDL_strstr(text, "] ")) text = SDL_strstr(text, "] ") + 2;
                post_of(l, GEAR_POST_MINE, false, false, SDL_strdup(text), out);
                given = true;
            }
        } else if (!quiet && talked_to && *l->text) {
            // Held for its turn's end; one held before it was a note along the way, given now.
            if (f->held) give_held(f, out), given = true;
            f->held = l, f->held_at = SDL_GetTicks(), l = NULL;
        } else if (*l->text) {  // (a line without text or files, such as a marker this client does not know, is passed over)
            post_of(l, GEAR_POST_MESSAGE, quiet, false, l->text, out);
            l->text = NULL, given = true;
        }
        free_line(l);
        save_position(f);
        if (given) return true;
    }
}
