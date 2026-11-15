#define _POSIX_C_SOURCE 200809L  // (fdopen and dup, for keeping standard output to the frames)
#include "serve.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
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
#include "pronounce.h"
#include "speakable.h"
#include "transcript.h"
#include "vad.h"

enum { SPEECH_RATE = 24000, MAX_AUDIO = 60 * 16000 };

typedef struct frame { struct frame *next; char type; Uint32 len; char *data; } frame;

// A message shown to the phone, numbered and kept (in hub-log.jsonl, so a restart keeps it too), for
// giving again to a phone that missed it.
typedef struct { long long seq; char *text; char who[128]; char at[8]; bool mine; } logged;
enum { LOG = 1000 };

typedef struct {
    // Frames from the client, read on a thread of their own from `from`; `closed` once its input ends.
    SDL_Mutex *lock;
    frame *in;
    bool closed;
    FILE *from;
    bool background;   // the phone's app is behind others: new replies go as text, not spoken
    // What goes out, one frame at a time, from the main thread and the synthesis thread.
    SDL_Mutex *out;
    // Speech: text queued for the synthesis thread, and the voice to load before the next.
    SDL_Condition *wake;
    char *speak, *next_voice;
    int *speak_from;  // for each byte of `speak`, its place in the reply (gear_pronounce_reply_map)
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

static FILE *frames_out;  // the client's end, NULL with none; what the engines print goes to standard error instead

static void send_frame(server *s, char type, const void *data, Uint32 len) {
    Uint8 head[5] = { (Uint8)type, (Uint8)len, (Uint8)(len >> 8), (Uint8)(len >> 16), (Uint8)(len >> 24) };
    SDL_LockMutex(s->out);
    if (!frames_out) { SDL_UnlockMutex(s->out); return; }
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
        if (fread(head, 1, 5, s->from) != 5) break;
        Uint32 len = (Uint32)head[1] | (Uint32)head[2] << 8 | (Uint32)head[3] << 16 | (Uint32)head[4] << 24;
        if (len > 8 * 1024 * 1024) break;  // (not a client of this protocol)
        frame *f = SDL_calloc(1, sizeof *f);
        f->type = (char)head[0], f->len = len, f->data = SDL_malloc(len + 1);
        if (len && fread(f->data, 1, len, s->from) != len) { SDL_free(f->data), SDL_free(f); break; }
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

typedef struct { server *s; int generation; long long samples; } job;

static bool on_chunk(const float *samples, int n, void *user) {
    job *j = user;
    if (SDL_GetAtomicInt(&j->s->generation) != j->generation) return false;
    j->samples += n;
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
        int *from = s->speak_from;
        s->next_voice = NULL, s->speak = NULL, s->speak_from = NULL;
        job j = { s, SDL_GetAtomicInt(&s->generation), 0 };
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
            // Sentence by sentence, each told to the phone before its speech ('L'): where in the reply it
            // was read from, and how far into the speech it starts, for the phone to show it as it is heard.
            size_t n = SDL_strlen(text);
            for (size_t a = 0; a < n && SDL_GetAtomicInt(&s->generation) == j.generation;) {
                while (a < n && text[a] == ' ') a++;
                if (a >= n) break;
                size_t e = a;
                while (e < n && !(SDL_strchr(".!?", text[e]) && (e + 1 == n || text[e + 1] == ' '))) e++;
                if (e < n) e++;
                size_t next = e;
                while (next < n && text[next] == ' ') next++;
                if (from) {
                    char line[96];
                    SDL_snprintf(line, sizeof line, "{\"start\":%d,\"end\":%d,\"at\":%.3f}", from[a], SDL_max(from[a], from[next]), (double)j.samples / SPEECH_RATE);
                    send_text(s, 'L', line);
                }
                char kept = text[e];
                text[e] = 0;
                struct pt_tts_params p;
                pt_tts_default_params(&p);
                p.text = text + a;
                p.voice = s->voice;
                p.on_chunk = on_chunk;
                p.user_data = &j;
                pt_synthesize(s->pocket, &p, NULL);
                text[e] = kept;
                a = next;
            }
            if (SDL_GetAtomicInt(&s->generation) == j.generation) send_frame(s, 'D', NULL, 0);
            SDL_free(text), SDL_free(from);
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
    int *from = NULL;
    char *spoken = gear_pronounce_reply_map(reply, &from);
    SDL_LockMutex(s->lock);
    // A new reply: its reference starts empty, as the phone's count of what it has played does.
    s->reply16_len = 0, s->phone_at = 0, s->since_at = 0;
    SDL_ClearAudioStream(s->to16);
    SDL_free(s->speak), SDL_free(s->speak_from);
    s->speak = spoken, s->speak_from = from;
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
    SDL_free(s->speak_from), s->speak_from = NULL;
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

// The title of the tab the agent `name` runs in, which names its conversation on the phone (and its
// Panetone route); the name itself when there is no such tab.
static void tab_of(const char *name, char *out, size_t size) {
    gear_agent all[256];
    char error[256];
    int n = gear_agents_list(all, 256, error, sizeof error);
    SDL_strlcpy(out, name, size);
    if (n <= 0 || !*name) return;
    gear_agent_tabs(all, n);
    for (int i = 0; i < n; i++)
        if (!SDL_strcmp(all[i].name, name) && all[i].tab[0]) SDL_strlcpy(out, all[i].tab, size);
}

static char target_tab[128];  // the conversation of the agent sent to now

static void hello(server *s, const char *target, const char *voice) {
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "target", gs_jval_str(target));
    gs_jval_set(o, "tab", gs_jval_str(target_tab));
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

static void log_message(server *s, const char *who, const char *at, bool mine, const char *text, bool shown);
static const char *conversation_of(const char *tab) { return *tab ? tab : "read back"; }

static void deliver_text(server *s, const char *text) {
    char status[200];
    send_text(s, 'M', text);
    log_message(s, conversation_of(target_tab), "", true, text, false);
    gear_transcript_add(out_to.transcript, text, out_to.target);
    if (*out_to.relay) {
        gear_relay_send(*out_to.relay, text);
        SDL_snprintf(status, sizeof status, "waiting for %s", out_to.target);
        send_text(s, 'S', status);
    } else send_text(s, 'R', text), log_message(s, "read back", "", false, text, false), speak(s, text);
}

// ---- What waits for the phone ----

static logged log_of[LOG];  // the newest LOG, oldest first
static int log_count;
static long long log_seq;
static char log_path[1100];

static void stamp(char at[8]) {
    SDL_Time t;
    SDL_DateTime d;
    at[0] = 0;
    if (SDL_GetCurrentTime(&t) && SDL_TimeToDateTime(t, &d, true)) SDL_snprintf(at, 8, "%02d:%02d", d.hour, d.minute);
}

static char *log_line(const logged *l) {
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "seq", gs_jval_num((double)l->seq));
    gs_jval_set(o, "who", gs_jval_str(l->who));
    gs_jval_set(o, "at", gs_jval_str(l->at));
    if (l->mine) gs_jval_set(o, "mine", gs_jval_bool(true));
    gs_jval_set(o, "text", gs_jval_str(l->text));
    char *json = gs_jval_write(o, false);
    gs_jval_free(o);
    return json;
}

static logged *log_add(long long seq, const char *who, const char *at, bool mine, const char *text) {
    if (log_count == LOG) SDL_free(log_of[0].text), SDL_memmove(log_of, log_of + 1, (LOG - 1) * sizeof *log_of), log_count--;
    logged *l = &log_of[log_count++];
    l->seq = seq, l->mine = mine, l->text = SDL_strdup(text);
    SDL_strlcpy(l->who, who, sizeof l->who), SDL_strlcpy(l->at, at, sizeof l->at);
    if (seq > log_seq) log_seq = seq;
    return l;
}

// The log as written, its newest LOG kept; written again without the older ones when it has grown
// past twice that.
static void load_log(void) {
    size_t n = 0;
    int lines = 0;
    char *all = SDL_LoadFile(log_path, &n);
    for (char *line = all, *end; line && line < all + n; line = end + 1) {
        if (!(end = SDL_strchr(line, '\n'))) end = all + n;
        gs_json *doc = gs_json_parse(line, (size_t)(end - line));
        if (!doc) continue;
        gs_jv o = gs_json_root(doc);
        const char *text = gs_json_str(gs_json_get(o, "text"), NULL);
        if (text)
            log_add((long long)gs_json_num(gs_json_get(o, "seq"), 0), gs_json_str(gs_json_get(o, "who"), ""), gs_json_str(gs_json_get(o, "at"), ""),
                    gs_json_bool(gs_json_get(o, "mine"), false), text), lines++;
        gs_json_free(doc);
    }
    SDL_free(all);
    if (lines <= 2 * LOG) return;
    SDL_IOStream *io = SDL_IOFromFile(log_path, "wb");
    for (int i = 0; io && i < log_count; i++) {
        char *line = log_line(&log_of[i]);
        SDL_WriteIO(io, line, SDL_strlen(line)), SDL_WriteIO(io, "\n", 1), SDL_free(line);
    }
    if (io) SDL_CloseIO(io);
}

static void send_seq(server *s, long long seq) {
    char n[32];
    SDL_snprintf(n, sizeof n, "%lld", seq);
    send_text(s, 'K', n);
}

// A message for the phone's conversation `who`, logged, and sent as 'Z' when a phone is connected
// and `shown`, followed by its number ('K'). (A reply also goes as 'R', and the user's message as 'M',
// before their numbers.)
static void log_message(server *s, const char *who, const char *at, bool mine, const char *text, bool shown) {
    char now[8];
    if (!*at) stamp(now), at = now;
    logged *l = log_add(log_seq + 1, who, at, mine, text);
    char *line = log_line(l);
    SDL_IOStream *io = SDL_IOFromFile(log_path, "ab");
    if (io) SDL_WriteIO(io, line, SDL_strlen(line)), SDL_WriteIO(io, "\n", 1), SDL_CloseIO(io);
    if (shown) send_text(s, 'Z', line);
    SDL_free(line);
    send_seq(s, l->seq);
}

// The messages after `seen`, to a phone that has had those up to it ('K'), as 'Z' and their numbers.
static void replay(server *s, long long seen) {
    for (int i = 0; i < log_count; i++) {
        if (log_of[i].seq <= seen) continue;
        // (marked as given again, which the phone shows and does not read aloud as new)
        char *line = log_line(&log_of[i]), *again = SDL_malloc(SDL_strlen(line) + 16);
        SDL_snprintf(again, SDL_strlen(line) + 16, "{\"again\":true,%s", line + 1);
        send_text(s, 'Z', again);
        SDL_free(line), SDL_free(again);
        send_seq(s, log_of[i].seq);
    }
}

// ---- Panetone's posts ----

// Everything Panetone posts to Mihai's own channels (`panetone watch`, one JSON line a post), read on a
// thread of its own and shown on the phone as an agent's message ('Z'), as Telegram shows it. The last
// position handled is kept in hub-feed-seq, so a restart repeats and skips nothing.
typedef struct post { struct post *next; char *text; char who[128]; char at[8]; long long seq; Uint64 came; } post;
static SDL_Mutex *posts_lock;
static post *posts;
static char seq_path[1100];

// A reply this hub has just given the phone itself, which Panetone posts too: those posts are not shown
// again. (Panetone splits a long post as Telegram takes it, so a post may be a part of one.)
static char *recent[16];
static int recent_at;

static void remember_reply(const char *text) {
    SDL_free(recent[recent_at]);
    recent[recent_at] = SDL_strdup(text);
    recent_at = (recent_at + 1) % 16;
}

static bool already_given(const char *text) {
    for (int i = 0; i < 16; i++)
        if (recent[i] && *text && SDL_strstr(recent[i], text)) return true;
    return false;
}

static int watch_feed(void *user) {
    (void)user;
    char *last = SDL_LoadFile(seq_path, NULL), cmd[200];
    if (last) SDL_snprintf(cmd, sizeof cmd, "panetone watch --after %lld", SDL_strtoll(last, NULL, 10));
    else SDL_strlcpy(cmd, "panetone watch", sizeof cmd);  // (the first time: from now on)
    SDL_free(last);
    FILE *in = popen(cmd, "r");
    if (!in) return 0;
    static char line[1 << 20];
    while (fgets(line, sizeof line, in)) {
        gs_json *doc = gs_json_parse(line, SDL_strlen(line));
        if (!doc) continue;
        gs_jv o = gs_json_root(doc);
        post *p = SDL_calloc(1, sizeof *p);
        p->came = SDL_GetTicks();
        p->seq = (long long)gs_json_num(gs_json_get(o, "seq"), 0);
        const char *route = gs_json_str(gs_json_get(o, "route"), NULL);
        SDL_strlcpy(p->who, route ? route : "panetone", sizeof p->who);
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
        SDL_LockMutex(posts_lock);
        post **q = &posts;
        while (*q) q = &(*q)->next;
        *q = p;
        SDL_UnlockMutex(posts_lock);
    }
    pclose(in);
    return 0;
}

// ---- The hub ----

#ifndef _WIN32
// The hub's socket: in the user's runtime folder, which only the user can enter.
static void hub_path(char *out, size_t size) {
    const char *dir = SDL_getenv("XDG_RUNTIME_DIR");
    char run[64];
    SDL_snprintf(run, sizeof run, "/run/user/%d", (int)getuid());
    // (systemd's runtime folder for the user when the variable is not set, as for some SSH commands)
    if (dir && *dir) SDL_snprintf(out, size, "%s/gear-hub.sock", dir);
    else if (access(run, W_OK) == 0) SDL_snprintf(out, size, "%s/gear-hub.sock", run);
    else SDL_snprintf(out, size, "/tmp/gear-hub-%d.sock", (int)getuid());
}

int gear_serve_proxy(void) {
    struct sockaddr_un at = { .sun_family = AF_UNIX };
    hub_path(at.sun_path, sizeof at.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&at, sizeof at) < 0) { if (fd >= 0) close(fd); return -1; }
    // Both ways until either end closes.
    char buf[65536];
    struct pollfd p[2] = { { .fd = 0, .events = POLLIN }, { .fd = fd, .events = POLLIN } };
    for (;;) {
        if (poll(p, 2, -1) < 0) break;
        if (p[0].revents) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0 || write(fd, buf, (size_t)n) != n) break;
        }
        if (p[1].revents) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0 || write(1, buf, (size_t)n) != n) break;
        }
    }
    close(fd);
    return 0;
}
#else
int gear_serve_proxy(void) { return -1; }
#endif

int gear_serve(const char *models, int hear_threads, int say_threads, bool hub) {
    char path[1200], name[128], error[512];
    server s = { 0 };
    int listener = -1, client = -1;
#ifndef _WIN32
    // The engines print progress to standard output in places: that goes to standard error, and the
    // frames to a copy of standard output kept for them, or to the phone connected to the hub.
    if (hub) {
        signal(SIGPIPE, SIG_IGN);  // (a phone gone mid-write is seen as the end of its input)
        // (and stopped by a signal at once, which SDL would turn into an event nobody reads: what waits
        // for the phone is already on disk)
        signal(SIGTERM, SIG_DFL), signal(SIGINT, SIG_DFL);
        struct sockaddr_un at = { .sun_family = AF_UNIX };
        hub_path(at.sun_path, sizeof at.sun_path);
        unlink(at.sun_path);
        listener = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener < 0 || bind(listener, (struct sockaddr *)&at, sizeof at) < 0 || listen(listener, 4) < 0) {
            fprintf(stderr, "gear: cannot listen at %s\n", at.sun_path);
            return 1;
        }
        frames_out = NULL, s.closed = true;
    } else {
        frames_out = fdopen(dup(1), "wb");
        s.from = stdin;
    }
    dup2(2, 1);
#else
    if (hub) { fprintf(stderr, "gear: the hub needs a Unix system\n"); return 1; }
    frames_out = stdout, s.from = stdin;
#endif
    {
        char *pref = SDL_GetPrefPath("wakamex", "gear");
        SDL_snprintf(log_path, sizeof log_path, "%shub-log.jsonl", pref ? pref : "");
        SDL_snprintf(seq_path, sizeof seq_path, "%shub-feed-seq", pref ? pref : "");
        SDL_free(pref);
        load_log();
    }
    posts_lock = SDL_CreateMutex();
    if (hub) SDL_DetachThread(SDL_CreateThread(watch_feed, "gear-hub-feed", NULL));
    gear_hear_model_pick(models, NULL, path, sizeof path, name, sizeof name);
    gear_hear *hear = gear_hear_open(path, hear_threads, error, sizeof error);
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
    // Agents sent to before the one now, still owing replies.
    typedef struct { gear_relay *relay; char name[128], tab[128]; } owed;
    enum { EARLIER = 16 };
    owed earlier[EARLIER];
    int earlier_count = 0;
    tab_of(target, target_tab, sizeof target_tab);
    SDL_Thread *reader = hub ? NULL : SDL_CreateThread(read_frames, "gear-serve-in", &s);
    SDL_Thread *speaker = SDL_CreateThread(synthesize, "gear-serve-say", &s);
    if (!hub) hello(&s, target, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");

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
#ifndef _WIN32
        // The hub: a phone connecting takes over from the one before, whose connection is closed.
        struct pollfd lp = { .fd = listener, .events = POLLIN };
        if (hub && poll(&lp, 1, 0) == 1) {
            int fd = accept(listener, NULL, NULL);
            if (fd >= 0) {
                if (client >= 0) shutdown(client, SHUT_RDWR);
                if (reader) SDL_WaitThread(reader, NULL), reader = NULL;
                SDL_LockMutex(s.out);
                if (frames_out) fclose(frames_out);
                frames_out = fdopen(dup(fd), "wb");
                SDL_UnlockMutex(s.out);
                if (s.from) fclose(s.from);
                if (client >= 0) close(client);
                client = fd, s.from = fdopen(dup(fd), "rb");
                SDL_LockMutex(s.lock);
                for (frame *g = s.in, *next; g; g = next) next = g->next, SDL_free(g->data), SDL_free(g);
                s.in = NULL, s.closed = false, s.background = false, s.phone_playing = false, s.stopping = false;
                SDL_UnlockMutex(s.lock);
                reader = SDL_CreateThread(read_frames, "gear-hub-in", &s);
                always = false, samples = 0, pending_n = 0;
                if (pipe) gear_pipeline_cancel(pipe);
                hello(&s, target, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");
            }
        }
#endif
        SDL_LockMutex(s.lock);
        frame *f = s.in;
        if (f) s.in = f->next;
        bool gone = !f && s.closed;
        open = !gone || hub;
        SDL_UnlockMutex(s.lock);
        // The hub with its phone gone: what it was saying stops, and replies wait for the next.
        if (hub && gone && frames_out) {
            SDL_AddAtomicInt(&s.generation, 1);
            SDL_LockMutex(s.out);
            fclose(frames_out), frames_out = NULL;
            SDL_UnlockMutex(s.out);
            always = false;
            if (pipe) gear_pipeline_cancel(pipe);
        }
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
            case 'B':  // the phone's app: "1" behind others, "0" in front again
                SDL_LockMutex(s.lock);
                s.background = f->data[0] == '1';
                SDL_UnlockMutex(s.lock);
                break;
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
            case 'M':  // a message typed: sent as one said
                if (*f->data) deliver_text(&s, f->data);
                break;
            case 'J': {  // a word to say rightly: how it is taught to be said, and CMUdict's way
                char said[128] = "", suggested[128] = "";
                gear_pronounce_get(f->data, said, sizeof said);
                gear_pronounce_suggest(models, f->data, suggested, sizeof suggested);
                gs_jval *o = gs_jval_object();
                gs_jval_set(o, "word", gs_jval_str(f->data));
                gs_jval_set(o, "say", gs_jval_str(said));
                gs_jval_set(o, "dictionary", gs_jval_str(suggested));
                char *json = gs_jval_write(o, false);
                send_text(&s, 'J', json);
                SDL_free(json), gs_jval_free(o);
                break;
            }
            case 'Y': {  // {"word", "say"} taught (an empty "say" forgets it), or "" for the list alone
                gs_json *doc = *f->data ? gs_json_parse(f->data, f->len) : NULL;
                if (doc) {
                    gs_jv o = gs_json_root(doc);
                    if (!gear_pronounce_set(gs_json_str(gs_json_get(o, "word"), ""), gs_json_str(gs_json_get(o, "say"), "")))
                        send_text(&s, 'F', "could not keep that pronunciation");
                    gs_json_free(doc);
                }
                char *list = gear_pronounce_list();
                send_text(&s, 'Y', list);
                SDL_free(list);
                break;
            }
            case 'U':  // a message to speak, in place of what is being spoken
                SDL_AddAtomicInt(&s.generation, 1);
                speak(&s, f->data);
                break;
            case 'L':
                list_agents(&s);
                break;
            case 'K':  // the phone has the messages up to this number: the rest again
                replay(&s, SDL_strtoll(f->data, NULL, 10));
                break;
            case 'T': {
                gear_agent a;
                if (!gear_agent_find(f->data, &a, error, sizeof error)) { send_text(&s, 'F', error); break; }
                // The agent before keeps its relay while it owes replies, which come to its own
                // conversation; one gone back to takes its relay up again.
                int was = -1;
                for (int i = 0; i < earlier_count; i++)
                    if (!SDL_strcmp(earlier[i].name, a.name)) was = i;
                gear_relay *again = was >= 0 ? earlier[was].relay : NULL;
                if (was >= 0) earlier[was] = earlier[--earlier_count];
                if (relay && gear_relay_waiting(relay) && earlier_count < EARLIER)
                    earlier[earlier_count++] = (owed){ relay, "", "" }, SDL_strlcpy(earlier[earlier_count - 1].name, target, 128),
                    SDL_strlcpy(earlier[earlier_count - 1].tab, target_tab, 128);
                else gear_relay_free(relay);
                relay = again ? again : gear_relay_new(a.name);
                SDL_strlcpy(target, a.name, sizeof target);
                tab_of(a.name, target_tab, sizeof target_tab);
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
        // Panetone's posts, to the phone or kept for it, the position written once each is handled. Each
        // waits 3 s first, for the relay to have the same reply when it is one this hub asked for.
        for (;;) {
            SDL_LockMutex(posts_lock);
            post *p = posts && SDL_GetTicks() - posts->came >= 3000 ? posts : NULL;
            if (p) posts = p->next;
            SDL_UnlockMutex(posts_lock);
            if (!p) break;
            if (!already_given(p->text)) {
                log_message(&s, p->who, p->at, false, p->text, true);
            }
            char seq[32];
            SDL_snprintf(seq, sizeof seq, "%lld", p->seq);
            SDL_SaveFile(seq_path, seq, SDL_strlen(seq));
            SDL_free(p->text), SDL_free(p);
        }
        bool failed;
        char *reply;
        for (int i = 0; i < earlier_count; i++) {
            while ((reply = gear_relay_take(earlier[i].relay, &failed, error, sizeof error)) || failed) {
                if (failed) { send_text(&s, 'F', error); continue; }
                remember_reply(reply);
                log_message(&s, earlier[i].tab, "", false, reply, true);
                SDL_free(reply);
            }
            if (!gear_relay_waiting(earlier[i].relay)) gear_relay_free(earlier[i].relay), earlier[i--] = earlier[--earlier_count];
        }
        while (relay && ((reply = gear_relay_take(relay, &failed, error, sizeof error)) || failed)) {
            if (failed) { send_text(&s, 'F', error); break; }
            // (logged for a phone that misses it; sent as text alone to one in the background)
            remember_reply(reply);
            send_text(&s, 'R', reply);
            log_message(&s, conversation_of(target_tab), "", false, reply, false);
            if (frames_out && !s.background) speak(&s, reply);
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
    if (reader) SDL_DetachThread(reader);  // (it is done, its input closed)
#ifndef _WIN32
    if (listener >= 0) close(listener);
#endif
    gear_relay_free(relay);
    for (int i = 0; i < earlier_count; i++) gear_relay_free(earlier[i].relay);
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
