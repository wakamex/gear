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
#include "panetone.h"
#include "pronounce.h"
#include "speakable.h"
#include "transcript.h"
#include "vad.h"

enum { SPEECH_RATE = 24000, MAX_AUDIO = 60 * 16000 };

typedef struct frame { struct frame *next; char type; Uint32 len; char *data; int from; } frame;  // from: the client it came from

// A message shown to the phone, numbered and kept (in hub-log.jsonl, so a restart keeps it too), for
// giving again to a phone that missed it.
// actions: a question's answers, "ID\tLABEL\n" each, or NULL; speaker and harness: in a room, the member that
// posted it and its agent's harness, or ""
typedef struct { long long seq; char *text, *actions; char who[128]; char at[8]; bool mine; bool quiet; char speaker[128], harness[32]; } logged;
enum { LOG = 1000 };

// A client: the phone, or the desktop's app, each connected to the hub at once. Its frames are read on
// a thread of its own from `from`; `closed` once its input ends.
enum { CLIENTS = 8 };
typedef struct {
    void *server;
    int index;
    FILE *out, *from;
    int fd;              // the hub's connection to it, or -1
    SDL_Thread *reader;
    bool live, closed;
    bool background;     // its app is behind others: new replies go to it as text, not spoken
    bool always;         // it asked for always-on listening
    Uint64 used;         // when it last talked, typed or asked for speech
    char target[128];    // the agent its open conversation sends to ('T'), or "" for the hub's
    char tab[128];       // and that agent's route, found once when it is chosen
    char *files;         // the files it sent ('F') for its next message, a line each, or NULL
} client;

typedef struct {
    // Frames from the clients, oldest first.
    SDL_Mutex *lock;
    frame *in;
    // The clients. Messages go to all of them; speech and what always-on hears go to and come from
    // `talker`, the one talked or typed on last (-1 for none). `only`, while not -1, sends the rest to
    // that one client alone (its greeting, and what it missed).
    client clients[CLIENTS];
    int talker, only;
    int skip;  // while not -1, the rest goes to every client but this one
    // What goes out, one frame at a time, from the main thread and the synthesis thread.
    SDL_Mutex *out;
    // Speech: text queued for the synthesis thread, and the voice to load before the next.
    SDL_Condition *wake;
    char *speak, *next_voice;
    int *speak_from;  // for each byte of `speak`, its place in the reply (gear_pronounce_reply_map)
    Uint32 speak_of;  // the reply's text, as text_id has it, for the phone to know which message is heard
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

// Frames of speech, of always-on's listening and of what the agent talked to is doing ('S'): for the
// voice client alone.
static bool voice_frame(char type) { return type && SDL_strchr("PLDIWS", type); }

// (what the engines print goes to standard error instead of a client)
static void send_frame(server *s, char type, const void *data, Uint32 len) {
    Uint8 head[5] = { (Uint8)type, (Uint8)len, (Uint8)(len >> 8), (Uint8)(len >> 16), (Uint8)(len >> 24) };
    SDL_LockMutex(s->out);
    for (int i = 0; i < CLIENTS; i++) {
        client *c = &s->clients[i];
        if (!c->out || (voice_frame(type) ? i != s->talker : s->only >= 0 ? i != s->only : i == s->skip)) continue;
        fwrite(head, 1, 5, c->out);
        if (len) fwrite(data, 1, len, c->out);
        fflush(c->out);
    }
    SDL_UnlockMutex(s->out);
}

static void send_text(server *s, char type, const char *text) { send_frame(s, type, text, (Uint32)SDL_strlen(text)); }

static int read_frames(void *user) {
    client *c = user;
    server *s = c->server;
    for (;;) {
        Uint8 head[5];
        if (fread(head, 1, 5, c->from) != 5) break;
        Uint32 len = (Uint32)head[1] | (Uint32)head[2] << 8 | (Uint32)head[3] << 16 | (Uint32)head[4] << 24;
        if (len > 8 * 1024 * 1024) break;  // (not a client of this protocol)
        frame *f = SDL_calloc(1, sizeof *f);
        f->type = (char)head[0], f->len = len, f->data = SDL_malloc(len + 1), f->from = c->index;
        if (len && fread(f->data, 1, len, c->from) != len) { SDL_free(f->data), SDL_free(f); break; }
        f->data[len] = 0;
        SDL_LockMutex(s->lock);
        frame **q = &s->in;
        while (*q) q = &(*q)->next;
        *q = f;
        SDL_UnlockMutex(s->lock);
    }
    SDL_LockMutex(s->lock);
    c->closed = true;
    SDL_UnlockMutex(s->lock);
    return 0;
}

// The voice client from now on, `which` (-1 for none): what was being said stops, as the device it
// played on is no longer the one in use.
static void use_voice(server *s, int which) {
    if (which == s->talker) return;
    SDL_AddAtomicInt(&s->generation, 1);
    SDL_LockMutex(s->out);
    s->talker = which;
    SDL_UnlockMutex(s->out);
    SDL_LockMutex(s->lock);
    SDL_free(s->speak), s->speak = NULL;
    SDL_free(s->speak_from), s->speak_from = NULL;
    s->phone_playing = false, s->stopping = false, s->phone_at = 0, s->since_at = 0, s->reply16_len = 0;
    SDL_UnlockMutex(s->lock);
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
        Uint32 of = s->speak_of;
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
                    SDL_snprintf(line, sizeof line, "{\"of\":%u,\"start\":%d,\"end\":%d,\"at\":%.3f}", (unsigned)of, from[a], SDL_max(from[a], from[next]), (double)j.samples / SPEECH_RATE);
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

// A text's identity, FNV-1a over its bytes, as the phone works it out for the message it shows.
static Uint32 text_id(const char *t) {
    Uint32 h = 2166136261u;
    for (; *t; t++) h = (h ^ (Uint8)*t) * 16777619u;
    return h;
}

// What is said of `reply` spoken: with the words taught their way, or with `as_written` without them.
static void speak_as(server *s, const char *reply, bool as_written) {
    int *from = NULL;
    char *spoken = as_written ? gear_speakable_map(reply, &from) : gear_pronounce_reply_map(reply, &from);
    SDL_LockMutex(s->lock);
    // A new reply: its reference starts empty, as the phone's count of what it has played does.
    s->reply16_len = 0, s->phone_at = 0, s->since_at = 0;
    SDL_ClearAudioStream(s->to16);
    SDL_free(s->speak), SDL_free(s->speak_from);
    s->speak = spoken, s->speak_from = from, s->speak_of = text_id(reply);
    SDL_SignalCondition(s->wake);
    SDL_UnlockMutex(s->lock);
}

static void speak(server *s, const char *reply) { speak_as(s, reply, false); }

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

// The route of the agent `name` (its tab's title), which names its conversation on the phone.
static void tab_of(const char *name, char *out, size_t size) {
    gear_agent a = { 0 };
    SDL_strlcpy(a.name, name, sizeof a.name);
    if (*name) gear_agent_route(&a, out, size);
    else SDL_strlcpy(out, name, size);
}

static char target_tab[128];  // the conversation of the agent sent to now

// The hub's settings, which every client shows and can change ('S'): the recognition model in use and
// how many there are to switch between, where it runs, whether the echo canceller is in this build, and
// always-on's countdown.
static struct { char hear[128]; int hears; char device[32]; bool echo; double countdown; } settings;

// (the tab found already: finding it asks Wakterm for every agent, too slow to do for each greeting)
static void hello(server *s, const char *target, const char *tab, const char *voice) {
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "target", gs_jval_str(target));
    gs_jval_set(o, "tab", gs_jval_str(tab));
    gs_jval_set(o, "voice", gs_jval_str(voice));
    gs_jval_set(o, "hear", gs_jval_str(settings.hear));
    gs_jval_set(o, "hears", gs_jval_num(settings.hears));
    gs_jval_set(o, "device", gs_jval_str(settings.device));
    gs_jval_set(o, "echo", gs_jval_bool(settings.echo));
    gs_jval_set(o, "countdown", gs_jval_num(settings.countdown));
    char *json = gs_jval_write(o, false);
    send_text(s, 'H', json);
    SDL_free(json);
    gs_jval_free(o);
}

// A message to the agent: shown to the phone, kept in the transcript, and handed to Panetone for the
// route of the agent's tab, as the user ('panetone say'), or read back when no agent is chosen. The
// agent's answers come through Panetone's feed (gear_feed).
typedef struct { const char *target; gear_transcript *transcript; } delivery;
static delivery out_to;

static void log_message(server *s, const char *who, const char *at, bool mine, bool quiet, const char *text, const char *actions, bool shown);
// The room member and harness the next message logged is from (log_message clears them).
static char next_speaker[128], next_harness[32];
static void send_seq(server *s, long long seq);
static long long log_seq;  // the newest message's number in the log
static const char *conversation_of(const char *tab) { return *tab ? tab : "read back"; }

// ---- Attachments ----

// The type of a file by its name's ending, as Panetone names the attachments it hands agents.
static const char *mime_of(const char *name) {
    static const char *const types[][2] = { { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
                                            { ".webp", "image/webp" }, { ".heic", "image/heic" }, { ".pdf", "application/pdf" }, { ".txt", "text/plain" },
                                            { ".md", "text/markdown" }, { ".csv", "text/csv" }, { ".json", "application/json" }, { ".mp4", "video/mp4" },
                                            { ".mov", "video/quicktime" }, { ".mp3", "audio/mpeg" }, { ".m4a", "audio/mp4" }, { ".wav", "audio/wav" },
                                            { ".ogg", "audio/ogg" }, { ".zip", "application/zip" } };
    const char *dot = SDL_strrchr(name, '.');
    for (size_t i = 0; dot && i < SDL_arraysize(types); i++)
        if (!SDL_strcasecmp(dot, types[i][0])) return types[i][1];
    return "application/octet-stream";
}

// A file a client sent ('F': its name, a zero byte, its bytes), kept on this computer in gear's data
// folder (attachments/DATE/TIME-NAME), and its line, "[attached TYPE: PATH]" as Panetone gives an agent
// a file sent from Telegram, added to what goes with the client's next message.
static void keep_file(server *s, client *c, const char *data, Uint32 len) {
    size_t name_len = SDL_strnlen(data, len);
    if (name_len >= len) return;
    char name[128] = "", at[1200], *pref = SDL_GetPrefPath("wakamex", "gear"), path[1400];
    for (size_t i = 0, n = 0; i < name_len && n + 1 < sizeof name; i++)  // (no folders, nor a name to hide, nor spaces in the path the agent reads)
        name[n++] = SDL_strchr("/\\:*?\"<>| ", data[i]) || (unsigned char)data[i] < 32 ? '_' : data[i], name[n] = 0;
    if (!*name || *name == '.') SDL_strlcpy(name, "file", sizeof name);
    SDL_Time now;
    SDL_DateTime d;
    SDL_GetCurrentTime(&now), SDL_TimeToDateTime(now, &d, true);
    SDL_snprintf(at, sizeof at, "%sattachments/%04d-%02d-%02d", pref ? pref : "", d.year, d.month, d.day);
    SDL_CreateDirectory(at);
    SDL_snprintf(path, sizeof path, "%s/%02d%02d%02d-%s", at, d.hour, d.minute, d.second, name);
    SDL_free(pref);
    if (!SDL_SaveFile(path, data + name_len + 1, len - name_len - 1)) { send_text(s, 'F', "could not keep an attachment"); return; }
    char line[1600];
    SDL_snprintf(line, sizeof line, "[attached %s: %s]", mime_of(name), path);
    size_t had = c->files ? SDL_strlen(c->files) : 0;
    c->files = SDL_realloc(c->files, had + SDL_strlen(line) + 2);
    SDL_snprintf(c->files + had, SDL_strlen(line) + 2, "%s\n", line);
}

static void deliver_text(server *s, const char *said) {
    char status[200], error[512];
    // (with the files the talking client sent for it after it, a line each)
    client *c = s->talker >= 0 ? &s->clients[s->talker] : NULL;
    char *with = NULL;
    if (c && c->files) {
        size_t n = SDL_strlen(said) + SDL_strlen(c->files) + 4;
        with = SDL_malloc(n);
        SDL_snprintf(with, n, "%s%s%s", said, *said ? "\n\n" : "", c->files);
        with[SDL_strlen(with) - 1] = 0;  // (the last line's newline)
        SDL_free(c->files), c->files = NULL;
    }
    const char *text = with ? with : said;
    // (as 'M' to the client it came from, the talking one; the others get it as a message of its
    // conversation, 'Z', so it goes there and not into the conversation they have open)
    if (s->talker >= 0) s->only = s->talker, send_text(s, 'M', text), s->only = -1;
    s->skip = s->talker;
    log_message(s, conversation_of(target_tab), "", true, false, text, NULL, true);
    s->skip = -1;
    if (s->talker >= 0) s->only = s->talker, send_seq(s, log_seq), s->only = -1;
    gear_transcript_add(out_to.transcript, text, out_to.target);
    if (*out_to.target) {
        if (gear_panetone_say(target_tab, text, "gear", error, sizeof error)) {
            SDL_snprintf(status, sizeof status, "waiting for %s", out_to.target);
            send_text(s, 'S', status);
        } else send_text(s, 'F', error);
    } else send_text(s, 'R', text), log_message(s, "read back", "", false, false, text, NULL, false), speak(s, text);
    SDL_free(with);
}

// ---- What waits for the phone ----

static logged log_of[LOG];  // the newest LOG, oldest first
static int log_count;
static long long log_seq;
static char log_path[1100], seq_path[1100];

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
    if (l->quiet) gs_jval_set(o, "quiet", gs_jval_bool(true));
    gs_jval_set(o, "text", gs_jval_str(l->text));
    if (*l->speaker) gs_jval_set(o, "speaker", gs_jval_str(l->speaker));
    if (*l->harness) gs_jval_set(o, "harness", gs_jval_str(l->harness));
    if (l->actions && *l->actions) {
        gs_jval *list = gs_jval_array();
        for (const char *a = l->actions; *a;) {
            const char *tab = SDL_strchr(a, '\t'), *end = SDL_strchr(a, '\n');
            if (!tab || !end || tab > end) break;
            char id[600], label[600];
            SDL_snprintf(id, sizeof id, "%.*s", (int)(tab - a), a), SDL_snprintf(label, sizeof label, "%.*s", (int)(end - tab - 1), tab + 1);
            gs_jval *one = gs_jval_object();
            gs_jval_set(one, "id", gs_jval_str(id)), gs_jval_set(one, "label", gs_jval_str(label));
            gs_jval_push(list, one);
            a = end + 1;
        }
        gs_jval_set(o, "actions", list);
    }
    char *json = gs_jval_write(o, false);
    gs_jval_free(o);
    return json;
}

static logged *log_add(long long seq, const char *who, const char *at, bool mine, bool quiet, const char *text, const char *actions) {
    if (log_count == LOG) SDL_free(log_of[0].text), SDL_free(log_of[0].actions), SDL_memmove(log_of, log_of + 1, (LOG - 1) * sizeof *log_of), log_count--;
    logged *l = &log_of[log_count++];
    l->seq = seq, l->mine = mine, l->quiet = quiet, l->text = SDL_strdup(text), l->actions = actions && *actions ? SDL_strdup(actions) : NULL;
    l->speaker[0] = l->harness[0] = 0;
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
        char actions[1536] = "";
        for (gs_jv a = gs_json_first(gs_json_get(o, "actions")); a; a = gs_json_next(a)) {
            char one[1210];
            SDL_snprintf(one, sizeof one, "%s\t%s\n", gs_json_str(gs_json_get(a, "id"), ""), gs_json_str(gs_json_get(a, "label"), ""));
            if (SDL_strlen(actions) + SDL_strlen(one) < sizeof actions) SDL_strlcat(actions, one, sizeof actions);
        }
        if (text) {
            logged *l = log_add((long long)gs_json_num(gs_json_get(o, "seq"), 0), gs_json_str(gs_json_get(o, "who"), ""), gs_json_str(gs_json_get(o, "at"), ""),
                                gs_json_bool(gs_json_get(o, "mine"), false), gs_json_bool(gs_json_get(o, "quiet"), false), text, actions);
            SDL_strlcpy(l->speaker, gs_json_str(gs_json_get(o, "speaker"), ""), sizeof l->speaker);
            SDL_strlcpy(l->harness, gs_json_str(gs_json_get(o, "harness"), ""), sizeof l->harness);
            lines++;
        }
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
static void log_message(server *s, const char *who, const char *at, bool mine, bool quiet, const char *text, const char *actions, bool shown) {
    char now[8];
    if (!*at) stamp(now), at = now;
    logged *l = log_add(log_seq + 1, who, at, mine, quiet, text, actions);
    SDL_strlcpy(l->speaker, next_speaker, sizeof l->speaker), SDL_strlcpy(l->harness, next_harness, sizeof l->harness);
    next_speaker[0] = next_harness[0] = 0;
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

// ---- Rooms ----

// Whether the route titled `route` is a room of member routes, a route with no agent of its own,
// asked of Panetone at most every five minutes for each route.
static bool room_route(const char *route) {
    enum { KNOWN = 128 };  // (more than the user's routes, so each is asked once in five minutes)
    static struct { char title[128]; bool room; Uint64 at; } known[KNOWN];
    static int next;
    for (int i = 0; i < KNOWN; i++)
        if (known[i].at && !SDL_strcmp(known[i].title, route) && SDL_GetTicks() - known[i].at < 300000) return known[i].room;
    int agents = 1;
    bool room = gear_panetone_route(route, &agents) && agents == 0;
    int i = next++ % KNOWN;
    SDL_strlcpy(known[i].title, route, sizeof known[i].title), known[i].room = room, known[i].at = SDL_GetTicks();
    return room;
}

// A room member's post, "NAME: text" with NAME its agent's name, as that member's: *speaker the member's
// route (its agent's tab, found once for each agent), and the text after the name. NULL when `text`
// is not of that form.
static const char *member_post(const char *text, char *speaker, size_t size) {
    const char *colon = SDL_strstr(text, ": ");
    if (!colon || colon == text || colon - text >= 100) return NULL;
    for (const char *c = text; c < colon; c++)
        if (!SDL_isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.') return NULL;
    char name[128];
    SDL_snprintf(name, sizeof name, "%.*s", (int)(colon - text), text);
    static struct { char name[128], tab[128]; } known[64];
    static int next;
    for (int i = 0; i < 64; i++)
        if (!SDL_strcmp(known[i].name, name)) { SDL_strlcpy(speaker, known[i].tab, size); return colon + 2; }
    int i = next++ % 64;
    SDL_strlcpy(known[i].name, name, sizeof known[i].name);
    tab_of(name, known[i].tab, sizeof known[i].tab);
    SDL_strlcpy(speaker, known[i].tab, size);
    return colon + 2;
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
    s.talker = s.only = s.skip = -1;
    int listener = -1;
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
    } else {
        s.clients[0] = (client){ &s, 0, fdopen(dup(1), "wb"), stdin, -1, NULL, true, false, false, false, 0 };
        s.talker = 0;
    }
    dup2(2, 1);
#else
    if (hub) { fprintf(stderr, "gear: the hub needs a Unix system\n"); return 1; }
    s.clients[0] = (client){ &s, 0, stdout, stdin, -1, NULL, true, false, false, false, 0 };
    s.talker = 0;
#endif
    {
        char *pref = SDL_GetPrefPath("wakamex", "gear");
        SDL_snprintf(log_path, sizeof log_path, "%shub-log.jsonl", pref ? pref : "");
        SDL_snprintf(seq_path, sizeof seq_path, "%shub-feed-seq", pref ? pref : "");
        SDL_free(pref);
        load_log();
    }
    // Everything Panetone posts to the user's channels, from where the hub last stopped (hub-feed-seq),
    // shown on the phone as Telegram shows it.
    gear_feed *feed = gear_feed_open(seq_path, "gear");
    gear_hear_model_pick(models, NULL, path, sizeof path, name, sizeof name);
    gear_hear *hear = gear_hear_open(path, hear_threads, error, sizeof error);
    s.lock = SDL_CreateMutex(), s.out = SDL_CreateMutex(), s.wake = SDL_CreateCondition();
    const SDL_AudioSpec speech = { SDL_AUDIO_F32, 1, SPEECH_RATE }, mic = { SDL_AUDIO_F32, 1, GEAR_RATE };
    s.to16 = SDL_CreateAudioStream(&speech, &mic);
    if (!hear) { send_text(&s, 'F', error); return 1; }
    {
        char names[16][128];
        SDL_strlcpy(settings.hear, name, sizeof settings.hear);
        settings.hears = gear_hear_models(models, names, 16);
        SDL_strlcpy(settings.device, gear_hear_device(hear), sizeof settings.device);
        settings.echo = gs_voice_has_echo();
        settings.countdown = gear_countdown_remembered(models, 3);
    }
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
    char *pref = SDL_GetPrefPath("wakamex", "gear");
    SDL_snprintf(path, sizeof path, "%smessages.jsonl", pref ? pref : "");
    SDL_free(pref);
    gear_transcript *transcript = gear_transcript_open(path, 1);
    out_to = (delivery){ target, transcript };
    tab_of(target, target_tab, sizeof target_tab);
    if (!hub) s.clients[0].reader = SDL_CreateThread(read_frames, "gear-serve-in", &s.clients[0]);
    SDL_Thread *speaker = SDL_CreateThread(synthesize, "gear-serve-say", &s);
    if (!hub) hello(&s, target, target_tab, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");

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
        // The hub: each phone or desktop connecting is a client beside the others (the voice one, when
        // there is none yet).
        struct pollfd lp = { .fd = listener, .events = POLLIN };
        if (hub && poll(&lp, 1, 0) == 1) {
            int fd = accept(listener, NULL, NULL), i = 0;
            while (fd >= 0 && i < CLIENTS && s.clients[i].live) i++;
            if (fd >= 0 && i == CLIENTS) close(fd);  // (a full hub turns the newest away)
            else if (fd >= 0) {
                client *c = &s.clients[i];
                SDL_LockMutex(s.out);
                *c = (client){ &s, i, fdopen(dup(fd), "wb"), fdopen(dup(fd), "rb"), fd, NULL, true, false, false, false, SDL_GetTicks() };
                SDL_UnlockMutex(s.out);
                c->reader = SDL_CreateThread(read_frames, "gear-hub-in", c);
                if (s.talker < 0) use_voice(&s, i);
                SDL_strlcpy(c->target, target, sizeof c->target), SDL_strlcpy(c->tab, target_tab, sizeof c->tab);
                s.only = i;
                hello(&s, c->target, c->tab, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");
                s.only = -1;
            }
        }
#endif
        SDL_LockMutex(s.lock);
        frame *f = s.in;
        if (f) s.in = f->next;
        SDL_UnlockMutex(s.lock);
        // A client gone, once its frames are handled: the others go on; the voice moves to the one
        // used last, and what was being said stops.
        for (int i = 0; !f && i < CLIENTS; i++) {
            client *c = &s.clients[i];
            SDL_LockMutex(s.lock);
            bool gone = c->live && c->closed;
            SDL_UnlockMutex(s.lock);
            if (!gone) continue;
            SDL_WaitThread(c->reader, NULL);
            SDL_LockMutex(s.out);
            if (c->out) fclose(c->out);
            c->out = NULL, c->live = false;
            SDL_UnlockMutex(s.out);
            if (c->from && c->from != stdin) fclose(c->from);
#ifndef _WIN32
            if (c->fd >= 0) close(c->fd);
#endif
            c->from = NULL, c->fd = -1;
            SDL_free(c->files), c->files = NULL;  // (files sent for a message that never came)
            if (i == s.talker) {
                int next = -1;
                for (int j = 0; j < CLIENTS; j++)
                    if (s.clients[j].live && (next < 0 || s.clients[j].used > s.clients[next].used)) next = j;
                use_voice(&s, next);
                always = next >= 0 && s.clients[next].always, samples = 0, pending_n = 0;
                if (pipe) gear_pipeline_cancel(pipe);
            }
        }
        open = hub || s.clients[0].live;
        if (f) {
            // (talking, typing or asking for speech on a client makes it the voice one)
            client *from = &s.clients[f->from];
            if (SDL_strchr("AEMUW", f->type)) {
                from->used = SDL_GetTicks();
                if (f->from != s.talker) {
                    use_voice(&s, f->from);
                    always = from->always, samples = 0, pending_n = 0;
                    if (pipe) gear_pipeline_cancel(pipe);
                    // (and what it says goes to the agent of its open conversation)
                    if (*from->target && SDL_strcmp(from->target, target)) {
                        SDL_strlcpy(target, from->target, sizeof target);
                        SDL_strlcpy(target_tab, from->tab, sizeof target_tab);
                        gear_target_remember(models, target, "");
                    }
                }
            }
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
            case 'O':  // always-on, "1" or "0": kept for the client, and heard when it is the voice one
                from->always = f->data[0] == '1';
                if (f->from != s.talker) break;
                always = from->always;
                if (always && !pipe) {
                    gear_model_path(models, GEAR_MODEL_VAD, path, sizeof path);
                    if (!(vad = gear_vad_open(path, error, sizeof error))) { send_text(&s, 'F', error); always = false; break; }
                    // (with WebRTC's echo canceller where this build has it, given what the phone plays)
                    voice_dsp = gs_voice_new(GEAR_RATE, GS_VOICE_SPEECH | (gs_voice_has_echo() ? GS_VOICE_ECHO : 0));
                    gear_listen_params lp = gear_listen_defaults();
                    pipe = gear_pipeline_new(voice_dsp, vad, hear, NULL, &lp, settings.countdown);
                    gear_pipeline_set_speaker(pipe, &(gear_speaker){ phone_busy, phone_stop, &s });
                    // Words as they are said, where the recogniser has a GPU (as the window's).
                    if (SDL_strcasecmp(gear_hear_device(hear), "cpu")) gear_pipeline_set_drafter(pipe, hear);
                }
                if (!always && pipe) gear_pipeline_cancel(pipe);
                pending_n = 0, samples = 0, last_words[0] = 0;
                break;
            case 'Q': {  // the phone's playback: "1 SECONDS" while it plays the reply, that far in; "0" when not
                if (f->from != s.talker) break;  // (another client plays nothing)
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
                from->background = f->data[0] == '1';
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
            case 'C': {  // one of a question's answers pressed: {"to": ROUTE, "action": ID}
                gs_json *doc = gs_json_parse(f->data, f->len);
                gs_jv o = doc ? gs_json_root(doc) : NULL;
                char label[600], error[600];
                if (o && !gear_panetone_answer(gs_json_str(gs_json_get(o, "to"), ""), gs_json_str(gs_json_get(o, "action"), ""), "gear", label, sizeof label, error, sizeof error))
                    send_text(&s, 'F', error);
                gs_json_free(doc);
                break;
            }
            case 'M':  // a message typed: sent as one said (with the files sent before it, if only those)
                if (*f->data || from->files) deliver_text(&s, f->data);
                break;
            case 'F':  // a file for the client's next message: kept here, and named in it
                keep_file(&s, from, f->data, f->len);
                break;
            case 'D': {  // a line from the phone, for finding a fault: kept in phone-debug.log
                char *pref = SDL_GetPrefPath("wakamex", "gear"), at[8], file[1100];
                SDL_snprintf(file, sizeof file, "%sphone-debug.log", pref ? pref : "");
                SDL_free(pref);
                stamp(at);
                SDL_IOStream *io = SDL_IOFromFile(file, "ab");
                if (io) SDL_IOprintf(io, "%s %s\n", at, f->data), SDL_CloseIO(io);
                break;
            }
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
            case 'W':  // the same, without the words taught: the speech's own way, to compare
                SDL_AddAtomicInt(&s.generation, 1);
                speak_as(&s, f->data, f->type == 'W');
                break;
            case 'L':
                list_agents(&s);
                break;
            case 'K':  // the phone has the messages up to this number: the rest again
                s.only = f->from;  // (to the client asking alone)
                replay(&s, SDL_strtoll(f->data, NULL, 10));
                s.only = -1;
                break;
            case 'T': {
                gear_agent a;
                s.only = f->from;  // (an answer, the greeting, to the client asking alone)
                // (no agent by that name or tab: a Panetone route of that title, such as a room of agents,
                // which has no tab of its own and is sent to as any route is)
                if (!gear_agent_find(f->data, &a, error, sizeof error)) {
                    if (!gear_panetone_route(f->data, NULL)) { send_text(&s, 'F', error); s.only = -1; break; }
                    a = (gear_agent){ 0 };
                    SDL_strlcpy(a.name, f->data, sizeof a.name), SDL_strlcpy(a.tab, f->data, sizeof a.tab);
                }
                // Each client sends to the agent of the conversation it has open; the hub's own is the
                // voice client's. (The agent before goes on answering in its own conversation, through the feed.)
                SDL_strlcpy(from->target, a.name, sizeof from->target);
                if (*a.tab) SDL_strlcpy(from->tab, a.tab, sizeof from->tab);
                else tab_of(a.name, from->tab, sizeof from->tab);
                if (f->from == s.talker) {
                    SDL_strlcpy(target, a.name, sizeof target);
                    SDL_strlcpy(target_tab, from->tab, sizeof target_tab);
                    gear_target_remember(models, a.name, "");
                }
                hello(&s, from->target, from->tab, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");
                s.only = -1;
                break;
            }
            case 'S': {  // a setting changed: {"countdown": SECONDS} or {"hear": "next"}, told to every client
                gs_json *doc = gs_json_parse(f->data, f->len);
                gs_jv o = doc ? gs_json_root(doc) : NULL;
                double secs = gs_json_num(gs_json_get(o, "countdown"), -1);
                if (secs >= 0) {
                    settings.countdown = SDL_clamp(secs, 0, 10);
                    gear_countdown_remember(models, settings.countdown);
                    if (pipe) gear_pipeline_set_countdown(pipe, settings.countdown);
                }
                // (the next recognition model in the models folder, loaded in place of the one in use)
                if (!SDL_strcmp(gs_json_str(gs_json_get(o, "hear"), ""), "next")) {
                    char names[16][128];
                    int n = gear_hear_models(models, names, 16), at = 0;
                    for (int i = 0; i < n; i++) if (!SDL_strcmp(names[i], settings.hear)) at = i;
                    gear_hear *h = NULL;
                    if (n > 1) {
                        SDL_snprintf(path, sizeof path, "%s/%s", models, names[(at + 1) % n]);
                        if (!(h = gear_hear_open(path, hear_threads, error, sizeof error))) send_text(&s, 'F', error);
                    }
                    if (h) {
                        gear_hear *old = hear;
                        hear = h;
                        if (pipe) {
                            gear_pipeline_set_hear(pipe, h, names[(at + 1) % n]);
                            // (drafts from the new model where it has a GPU, and none on the CPU, as when opened)
                            gear_pipeline_set_drafter(pipe, SDL_strcasecmp(gear_hear_device(h), "cpu") ? h : NULL);
                        }
                        gear_hear_close(old);
                        SDL_strlcpy(settings.hear, names[(at + 1) % n], sizeof settings.hear);
                        SDL_strlcpy(settings.device, gear_hear_device(h), sizeof settings.device);
                        gear_hear_model_remember(models, settings.hear);
                    }
                }
                gs_json_free(doc);
                for (int i = 0; i < CLIENTS; i++)  // (each with its own agent)
                    if (s.clients[i].live)
                        s.only = i, hello(&s, *s.clients[i].target ? s.clients[i].target : target, *s.clients[i].target ? s.clients[i].tab : target_tab, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");
                s.only = -1;
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
                for (int i = 0; i < CLIENTS; i++)  // (each with its own agent)
                    if (s.clients[i].live)
                        s.only = i, hello(&s, *s.clients[i].target ? s.clients[i].target : target, *s.clients[i].target ? s.clients[i].tab : target_tab, voice == GEAR_MODEL_VOICE_MARY ? "mary" : "alba");
                s.only = -1;
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
        // Panetone's posts, to the phone or kept for it: what the agent sent to now posts to end its
        // turn is its reply, shown and spoken (as text alone to a phone in the background).
        for (gear_post p; feed && gear_feed_next(feed, *target ? target_tab : "", &p); SDL_free(p.text)) {
            // (in a room, each post shown as its speaker's, without the "NAME: " Panetone puts before it: a
            // member's with its agent's harness, and Panetone's or a person's with none)
            char speaker[128] = "";
            const char *said = room_route(p.who) ? member_post(p.text, speaker, sizeof speaker) : NULL;
            if (said) {
                SDL_memmove(p.text, said, SDL_strlen(said) + 1);
                SDL_strlcpy(next_speaker, speaker, sizeof next_speaker), SDL_strlcpy(next_harness, p.harness, sizeof next_harness);
            }
            if (p.kind == GEAR_POST_REPLY) {
                // The reply as 'R' to the client talking to its agent alone, whose conversation it is; the
                // others, which may have another conversation open, get it as a message of its route ('Z'),
                // as does the talking client too when it has answers to press, which 'Z' carries.
                if (!p.shown) {
                    if (s.talker >= 0) s.only = s.talker, send_text(&s, 'R', p.text), s.only = -1;
                    s.skip = *p.actions ? -1 : s.talker;
                    log_message(&s, p.who, p.at, false, false, p.text, p.actions, true);
                    s.skip = -1;
                    if (s.talker >= 0 && !*p.actions) s.only = s.talker, send_seq(&s, log_seq), s.only = -1;  // (its number)
                }
                if (s.talker >= 0 && !s.clients[s.talker].background) speak(&s, p.text);
            } else log_message(&s, p.who, p.at, p.kind == GEAR_POST_MINE, p.quiet, p.text, p.actions, true);
            next_speaker[0] = next_harness[0] = 0;  // (not left for another message when this one was only spoken)
        }
        SDL_Delay(10);
    }
    SDL_LockMutex(s.lock);
    s.quit = true;
    SDL_AddAtomicInt(&s.generation, 1);
    SDL_SignalCondition(s.wake);
    SDL_UnlockMutex(s.lock);
    SDL_WaitThread(speaker, NULL);
    for (int i = 0; i < CLIENTS; i++)
        if (s.clients[i].reader) SDL_DetachThread(s.clients[i].reader);  // (done, or ended with the process)
#ifndef _WIN32
    if (listener >= 0) close(listener);
#endif
    gear_feed_close(feed);
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
