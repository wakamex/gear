// gear on a phone: a character to hold while talking. Held, it listens and what is said goes to gear on
// the computer over SSH; let go, it is sent to the agent, and the agent's reply is spoken back. The
// computer does the hearing, the agent and the speech (src/serve.c); the phone records, plays and draws.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include "gs_draw.h"
#include "gs_background.h"
#include "gs_json.h"
#include "gs_notify.h"
#include "gs_mix.h"
#include "gs_secret.h"
#include "gs_stream.h"
#include "gs_text.h"
#include "gs_ui.h"
#include "key.h"
#include "link.h"
#include "rich.h"

enum { MIX_RATE = 48000, MIC_RATE = 16000, SPEECH_RATE = 24000, MAX_AGENTS = 200 };

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

typedef enum { IDLE, LISTENING, THINKING, SPEAKING } mood;

// The conversations, both sides, kept on the phone in history.jsonl so they outlast a restart: what was
// sent and what each agent replied, with when, newest last. A conversation is one agent's, named by
// the title of its tab (`who`), as Telegram has one topic for each.
typedef struct { char *text; char who[128]; char at[8]; bool mine; } entry;
enum { HISTORY = 2000, CONVERSATIONS = 64 };
typedef struct { char who[128]; int n; } unread;  // the replies in a conversation not yet seen

typedef struct {
    // Where gear runs, kept in settings.txt; the phone's key, kept with gs_secret.
    char host[256], user[64], port[8], host_key[128];
    bool always;          // always on: listening without holding, the computer finding the messages
    char avatar[16];      // the character drawn: fox, tomato, girl or robot
    char pem[1024], key_line[400];
    char dir[1024];
    gp_link *link;
    bool setup;           // the settings screen is showing
    char status[300];     // the connection's state, or why it failed
    // The conversations: the one shown, the agent sent to (its name, and its tab's title naming its
    // conversation), and the agent last asked for, once per conversation opened.
    char open[128], target[128], tab[128], asked[128], voice[16], note[300];
    unread unread[CONVERSATIONS];
    long long seen;       // the newest message's number in the computer's log, kept in seen.txt
    int unread_count;
    char typed[4000];     // the message being typed
    char *said, *reply;   // what is being said now (always-on's live words), and the reply being spoken
    entry history[HISTORY];
    int count;
    bool fresh;           // a message came: the conversation scrolls to its start
    mood mood;
    bool holding, pressing;  // a hold under way; a press on the character not yet a hold
    Uint64 held_at;
    bool always_sent;     // the computer has been told always-on is on, on this connection
    double left;          // always-on: seconds before what is said is sent, or < 0
    float level;          // the microphone's level while held, 0 to 1
    SDL_AudioStream *mic;
    bool mic_allowed;
    // The reply's speech: 24 kHz mono from the computer, to the mix's rate in stereo, and played.
    SDL_AudioStream *conv;
    gs_stream *play;
    bool reply_done;
    Uint64 audio_at;      // when the last of the reply's speech came
    const char *saying;   // the text of the message being spoken (a history entry's), or NULL
    double reply_base;    // where the reply being played starts in the player's count of seconds
    // Words taught to be said rightly (pronunciation_panel): the word, as it is said, CMUdict's way, and the
    // words taught, from the computer.
    bool teaching;
    char word[64], say_as[128], dictionary[128], looked_up[64];
    Uint64 word_typed;
    bool say_as_came;     // the computer filled "say it as" in: the field told so when it is next drawn
    char taught[200][2][128];
    int taught_count;
    // The agents to pick from.
    bool picking;
    char agents[MAX_AGENTS][2][128];  // name, and the tab's title
    bool busy[MAX_AGENTS];            // in the middle of a turn
    int agent_count;
    Uint64 asked_agents;              // when the list was last asked for, to keep it current while shown
} app;

// ---- Settings ----

static void settings_path(app *a, char *out, size_t size, const char *name) { SDL_snprintf(out, size, "%s%s", a->dir, name); }

static void load_settings(app *a) {
    char path[1100];
    settings_path(a, path, sizeof path, "settings.txt");
    char *text = SDL_LoadFile(path, NULL);
    SDL_strlcpy(a->port, "22", sizeof a->port);
    SDL_strlcpy(a->avatar, "tomato", sizeof a->avatar);
    char *save = NULL;
    for (char *line = text ? SDL_strtok_r(text, "\n", &save) : NULL; line; line = SDL_strtok_r(NULL, "\n", &save)) {
        char *eq = SDL_strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!SDL_strcmp(line, "host")) SDL_strlcpy(a->host, v, sizeof a->host);
        else if (!SDL_strcmp(line, "user")) SDL_strlcpy(a->user, v, sizeof a->user);
        else if (!SDL_strcmp(line, "port")) SDL_strlcpy(a->port, v, sizeof a->port);
        else if (!SDL_strcmp(line, "host_key")) SDL_strlcpy(a->host_key, v, sizeof a->host_key);
        else if (!SDL_strcmp(line, "always")) a->always = *v == '1';
        else if (!SDL_strcmp(line, "avatar")) SDL_strlcpy(a->avatar, v, sizeof a->avatar);
        else if (!SDL_strcmp(line, "open")) SDL_strlcpy(a->open, v, sizeof a->open);
    }
    SDL_free(text);
}

static void save_settings(app *a) {
    char path[1100], text[900];
    settings_path(a, path, sizeof path, "settings.txt");
    SDL_snprintf(text, sizeof text, "host=%s\nuser=%s\nport=%s\nhost_key=%s\nalways=%d\navatar=%s\nopen=%s\n", a->host, a->user, a->port, a->host_key, a->always, a->avatar, a->open);
    SDL_SaveFile(path, text, SDL_strlen(text));
}

static void remember(app *a, const char *text, const char *who, const char *at, bool mine) {
    if (a->count == HISTORY) SDL_free(a->history[0].text), SDL_memmove(a->history, a->history + 1, (HISTORY - 1) * sizeof *a->history), a->count--;
    entry *e = &a->history[a->count++];
    e->text = SDL_strdup(text), e->mine = mine;
    SDL_strlcpy(e->who, who, sizeof e->who), SDL_strlcpy(e->at, at, sizeof e->at);
}

static void load_history(app *a) {
    char path[1100];
    settings_path(a, path, sizeof path, "history.jsonl");
    size_t n = 0;
    char *all = SDL_LoadFile(path, &n);
    for (char *line = all, *end; line && line < all + n; line = end + 1) {
        if (!(end = SDL_strchr(line, '\n'))) end = all + n;
        gs_json *doc = gs_json_parse(line, (size_t)(end - line));
        if (!doc) continue;
        gs_jv o = gs_json_root(doc);
        const char *text = gs_json_str(gs_json_get(o, "text"), NULL);
        if (text) remember(a, text, gs_json_str(gs_json_get(o, "who"), ""), gs_json_str(gs_json_get(o, "at"), ""), gs_json_bool(gs_json_get(o, "mine"), false));
        gs_json_free(doc);
    }
    SDL_free(all);
}

// ---- Conversations ----

// The conversation of the agent sent to now.
static const char *here(app *a) { return a->tab[0] ? a->tab : a->target[0] ? a->target : "read back"; }

// The count of a conversation's unseen replies, kept in unread.txt as "COUNT WHO" lines.
static int *unread_of(app *a, const char *who, bool make) {
    for (int i = 0; i < a->unread_count; i++)
        if (!SDL_strcmp(a->unread[i].who, who)) return &a->unread[i].n;
    if (!make || a->unread_count == CONVERSATIONS) return NULL;
    unread *u = &a->unread[a->unread_count++];
    SDL_strlcpy(u->who, who, sizeof u->who), u->n = 0;
    return &u->n;
}

static void save_unread(app *a) {
    char path[1100], text[CONVERSATIONS * 140] = "";
    settings_path(a, path, sizeof path, "unread.txt");
    for (int i = 0; i < a->unread_count; i++)
        if (a->unread[i].n) SDL_snprintf(text + SDL_strlen(text), sizeof text - SDL_strlen(text), "%d %s\n", a->unread[i].n, a->unread[i].who);
    SDL_SaveFile(path, text, SDL_strlen(text));
}

static void load_unread(app *a) {
    char path[1100], *save = NULL;
    settings_path(a, path, sizeof path, "unread.txt");
    char *text = SDL_LoadFile(path, NULL);
    for (char *line = text ? SDL_strtok_r(text, "\n", &save) : NULL; line; line = SDL_strtok_r(NULL, "\n", &save)) {
        char *space = SDL_strchr(line, ' ');
        int *n = space ? unread_of(a, space + 1, true) : NULL;
        if (n) *n = SDL_atoi(line);
    }
    SDL_free(text);
}

// Shows the conversation `who`, its replies seen and its notification taken back, and sends to its
// agent from now on (asked of the computer when connected, and again on connecting).
static void open_conversation(app *a, const char *who) {
    SDL_strlcpy(a->open, who, sizeof a->open);
    int *n = unread_of(a, who, false);
    if (n && *n) *n = 0, save_unread(a);
    gs_notify_close(who);
    save_settings(a);
    a->asked[0] = 0;
    if (a->link && gp_link_status(a->link, NULL, 0) == GP_OPEN && SDL_strcmp(who, here(a)) && SDL_strcmp(who, "read back"))
        gp_link_send(a->link, 'T', who, (uint32_t)SDL_strlen(who)), SDL_strlcpy(a->asked, who, sizeof a->asked);
}

// A message of a conversation, kept and shown: what the user sent (to `who`), or an agent's reply,
// counted unseen when its conversation is not the one open.
static void add_message_at(app *a, const char *text, const char *who, const char *at, bool mine) {
    remember(a, text, who, at, mine);
    if (!SDL_strcmp(who, a->open)) a->fresh = true;
    else if (!mine) {
        int *n = unread_of(a, who, true);
        if (n) ++*n, save_unread(a);
    }
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "at", gs_jval_str(at));
    gs_jval_set(o, "who", gs_jval_str(who));
    gs_jval_set(o, "mine", gs_jval_bool(mine));
    gs_jval_set(o, "text", gs_jval_str(text));
    char *line = gs_jval_write(o, false), path[1100];
    gs_jval_free(o);
    settings_path(a, path, sizeof path, "history.jsonl");
    SDL_IOStream *io = line ? SDL_IOFromFile(path, "ab") : NULL;
    if (io) SDL_WriteIO(io, line, SDL_strlen(line)), SDL_WriteIO(io, "\n", 1), SDL_CloseIO(io);
    SDL_free(line);
}

static void add_message(app *a, const char *text, const char *who, bool mine) {
    SDL_Time now;
    SDL_DateTime d;
    char at[8] = "";
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &d, true)) SDL_snprintf(at, sizeof at, "%02d:%02d", d.hour, d.minute);
    add_message_at(a, text, who, at, mine);
}

// ---- Notifications ----

// While the app is behind others its loop is paused, so the link's own thread tells of a reply, or of
// what an agent posted, with a notification; a tap brings the app back.
static SDL_AtomicInt behind;
static SDL_Mutex *who_lock;
static char who_now[128];  // the conversation sent to now, for a reply's notification
static char tapped[128];   // the conversation of a notification tapped, for the app to open

static void heard(char type, const char *data, uint32_t len, void *user) {
    (void)user;
    if (!SDL_GetAtomicInt(&behind) || (type != 'R' && type != 'Z')) return;
    char title[160], body[300];
    if (type == 'R') {
        SDL_LockMutex(who_lock);
        SDL_strlcpy(title, who_now[0] ? who_now : "gear", sizeof title);
        SDL_UnlockMutex(who_lock);
        SDL_strlcpy(body, data, sizeof body);
    } else {
        gs_json *doc = gs_json_parse(data, len);
        if (!doc) return;
        SDL_strlcpy(title, gs_json_str(gs_json_get(gs_json_root(doc), "who"), "gear"), sizeof title);
        SDL_strlcpy(body, gs_json_str(gs_json_get(gs_json_root(doc), "text"), ""), sizeof body);
        gs_json_free(doc);
    }
    gs_notify_show(title, title, body);  // (one for each conversation, its newest message)
}

static void notification_clicked(const char *tag) {
    SDL_LockMutex(who_lock);
    SDL_strlcpy(tapped, tag, sizeof tapped);
    SDL_UnlockMutex(who_lock);
}

// The phone's key: made the first time, then kept where the system keeps secrets.
static bool load_key(app *a) {
    char path[1100];
    settings_path(a, path, sizeof path, "key");
    char *pem = gs_secret_load(path, NULL);
    if (pem) SDL_strlcpy(a->pem, pem, sizeof a->pem), SDL_free(pem);
    else if (!gp_key_new(a->pem, sizeof a->pem) || !gs_secret_save(path, a->pem, SDL_strlen(a->pem))) return false;
    char pub[300];
    if (!gp_key_public(a->pem, "gear-phone", pub, sizeof pub)) return false;
    // As it goes in authorized_keys: the key can run gear's server mode and nothing else.
    SDL_snprintf(a->key_line, sizeof a->key_line, "command=\"gear serve\",restrict %s", pub);
    // The same line in a plain file, for reading it off the phone (adb's run-as) instead of copying it.
    settings_path(a, path, sizeof path, "key.pub");
    SDL_SaveFile(path, a->key_line, SDL_strlen(a->key_line));
    return true;
}

// ---- The connection ----

static void connect_link(app *a) {
    gp_link_close(a->link);
    a->link = NULL;
    if (!a->host[0] || !a->user[0]) return;
    gp_link_config c = { 0 };
    SDL_strlcpy(c.host, a->host, sizeof c.host);
    c.port = SDL_atoi(a->port) > 0 ? SDL_atoi(a->port) : 22;
    SDL_strlcpy(c.user, a->user, sizeof c.user);
    char pub[300];
    gp_key_public(a->pem, "gear-phone", pub, sizeof pub);
    c.key_pem = a->pem, c.key_public = pub;
    SDL_strlcpy(c.host_key, a->host_key, sizeof c.host_key);
    c.seen = a->seen;
    a->link = gp_link_open(&c);
    gp_link_on_frame(a->link, heard, NULL);
    // Online behind other apps too, as gink is (asked while in front, as Android wants).
    static bool kept;
    if (!kept) kept = gs_background_keep("gear", "Connected to your agents");
    a->mood = IDLE;
    a->always_sent = false;
}

static void set_text(char **slot, const char *text) { SDL_free(*slot), *slot = SDL_strdup(text); }

static void parse_agents(app *a, const char *json) {
    gs_json *doc = gs_json_parse(json, SDL_strlen(json));
    a->agent_count = 0;
    for (gs_jv x = doc ? gs_json_first(gs_json_root(doc)) : NULL; x && a->agent_count < MAX_AGENTS; x = gs_json_next(x)) {
        SDL_strlcpy(a->agents[a->agent_count][0], gs_json_str(gs_json_get(x, "name"), ""), 128);
        SDL_strlcpy(a->agents[a->agent_count][1], gs_json_str(gs_json_get(x, "tab"), ""), 128);
        a->busy[a->agent_count] = !SDL_strcmp(gs_json_str(gs_json_get(x, "status"), ""), "busy");
        a->agent_count++;
    }
    gs_json_free(doc);
}

// The reply's speech comes far faster than it plays (a minute of it in two seconds), and with no end
// to how long a reply is read: it waits as it came, 16-bit at 24 kHz, and goes to the player a few
// seconds ahead of what is heard.
static void feed_speech(app *a) {
    float buf[4096];
    for (int got; gs_stream_buffered(a->play) < 4 && (got = SDL_GetAudioStreamData(a->conv, buf, sizeof buf)) > 0;)
        gs_stream_write(a->play, buf, got / (int)(2 * sizeof(float)));
}

// Seconds of the reply's speech still to be heard.
static double speech_left(app *a) { return gs_stream_buffered(a->play) + SDL_GetAudioStreamQueued(a->conv) / (2.0 * SPEECH_RATE); }

static void stop_speaking(app *a) {
    gs_stream_flush(a->play);
    SDL_ClearAudioStream(a->conv);
    a->saying = NULL;
}

// A message's speaker tapped: the message spoken, in place of what was; tapped while it is spoken, stopped.
static void say_again(app *a, const entry *e) {
    if (!a->link || gp_link_status(a->link, NULL, 0) != GP_OPEN) return;
    bool same = a->saying == e->text;
    if (a->saying || a->mood == SPEAKING) gp_link_send(a->link, 'X', NULL, 0), stop_speaking(a), a->mood = IDLE;
    if (same) return;
    gp_link_send(a->link, 'U', e->text, (uint32_t)SDL_strlen(e->text));
    a->saying = e->text, a->reply_done = false, a->audio_at = SDL_GetTicks();  // (its speech has 2 s to start)
    a->reply_base = gs_stream_position(a->play) + speech_left(a);
}

static void take_frames(app *a) {
    char type, *data;
    uint32_t len;
    while (a->link && gp_link_take(a->link, &type, &data, &len)) {
        switch (type) {
        case 'H': {
            gs_json *doc = gs_json_parse(data, len);
            if (doc) {
                SDL_strlcpy(a->target, gs_json_str(gs_json_get(gs_json_root(doc), "target"), ""), sizeof a->target);
                SDL_strlcpy(a->tab, gs_json_str(gs_json_get(gs_json_root(doc), "tab"), ""), sizeof a->tab);
                SDL_strlcpy(a->voice, gs_json_str(gs_json_get(gs_json_root(doc), "voice"), "alba"), sizeof a->voice);
            }
            gs_json_free(doc);
            if (a->always && !a->always_sent) gp_link_send(a->link, 'O', "1", 1), a->always_sent = true;
            // The conversation open sends to its agent, asked for once; with none open, the agent's.
            if (!a->open[0]) open_conversation(a, here(a));
            else if (SDL_strcmp(a->open, here(a)) && SDL_strcmp(a->open, "read back") && SDL_strcmp(a->asked, a->open))
                gp_link_send(a->link, 'T', a->open, (uint32_t)SDL_strlen(a->open)), SDL_strlcpy(a->asked, a->open, sizeof a->asked);
            SDL_LockMutex(who_lock);
            SDL_strlcpy(who_now, here(a), sizeof who_now);
            SDL_UnlockMutex(who_lock);
            break;
        }
        case 'Z': {  // a message to show and not speak: an agent's post, a reply from an agent before, one missed
            gs_json *doc = gs_json_parse(data, len);
            if (doc) {
                gs_jv o = gs_json_root(doc);
                const char *text = gs_json_str(gs_json_get(o, "text"), ""), *who = gs_json_str(gs_json_get(o, "who"), "");
                bool mine = gs_json_bool(gs_json_get(o, "mine"), false);
                // (one shown already, its number lost with a connection that broke before it came or come
                // before the phone kept numbers, is not shown twice)
                bool had = false;
                for (int i = a->count - 1; i >= 0 && i >= a->count - 200; i--)
                    if (a->history[i].mine == mine && !SDL_strcmp(a->history[i].who, who) && !SDL_strcmp(a->history[i].text, text)) had = true;
                if (!had) add_message_at(a, text, who, gs_json_str(gs_json_get(o, "at"), ""), mine);
            }
            gs_json_free(doc);
            break;
        }
        case 'K': {  // the number of the message just come, kept for asking for what follows it
            long long seq = SDL_strtoll(data, NULL, 10);
            if (seq > a->seen) {
                a->seen = seq;
                char path[1100];
                settings_path(a, path, sizeof path, "seen.txt");
                SDL_SaveFile(path, data, len);
            }
            break;
        }
        case 'W': {  // always-on: what is being said, and the countdown
            gs_json *doc = gs_json_parse(data, len);
            if (doc) {
                gs_jv o = gs_json_root(doc);
                const char *text = gs_json_str(gs_json_get(o, "text"), ""), *draft = gs_json_str(gs_json_get(o, "draft"), "");
                char *both = SDL_malloc(SDL_strlen(text) + SDL_strlen(draft) + 2);
                SDL_snprintf(both, SDL_strlen(text) + SDL_strlen(draft) + 2, "%s%s%s", text, *text && *draft ? " " : "", draft);
                if (*both) set_text(&a->said, both);
                SDL_free(both);
                a->left = gs_json_num(gs_json_get(o, "left"), -1);
            }
            gs_json_free(doc);
            break;
        }
        case 'M':
            add_message(a, data, here(a), true);
            SDL_free(a->said), a->said = NULL, a->note[0] = 0, a->left = -1;
            if (a->always) a->mood = THINKING;
            break;
        case 'R':
            set_text(&a->reply, data), a->reply_done = false, a->note[0] = 0, a->audio_at = SDL_GetTicks();
            add_message(a, data, here(a), false);
            if (!SDL_GetAtomicInt(&behind)) a->saying = a->history[a->count - 1].text;  // (spoken as it comes)
            a->reply_base = gs_stream_position(a->play) + speech_left(a);
            break;
        case 'I': stop_speaking(a), a->mood = IDLE; break;  // talked over: the computer stopped it
        case 'S': SDL_strlcpy(a->note, data, sizeof a->note); break;
        case 'F': SDL_strlcpy(a->note, data, sizeof a->note), a->mood = IDLE; break;
        case 'G': parse_agents(a, data); break;
        case 'J': {  // how a word is said: as taught, and CMUdict's way
            gs_json *doc = gs_json_parse(data, len);
            if (doc && !SDL_strcasecmp(gs_json_str(gs_json_get(gs_json_root(doc), "word"), ""), a->word)) {
                gs_jv o = gs_json_root(doc);
                SDL_strlcpy(a->dictionary, gs_json_str(gs_json_get(o, "dictionary"), ""), sizeof a->dictionary);
                const char *said = gs_json_str(gs_json_get(o, "say"), "");
                if (*said) SDL_strlcpy(a->say_as, said, sizeof a->say_as), a->say_as_came = true;
            }
            gs_json_free(doc);
            break;
        }
        case 'Y': {  // the words taught
            gs_json *doc = gs_json_parse(data, len);
            a->taught_count = 0;
            for (gs_jv x = doc ? gs_json_first(gs_json_root(doc)) : NULL; x && a->taught_count < 200; x = gs_json_next(x), a->taught_count++) {
                SDL_strlcpy(a->taught[a->taught_count][0], gs_json_str(gs_json_get(x, "word"), ""), 128);
                SDL_strlcpy(a->taught[a->taught_count][1], gs_json_str(gs_json_get(x, "say"), ""), 128);
            }
            gs_json_free(doc);
            break;
        }
        case 'D': a->reply_done = true; break;
        case 'P': {
            SDL_PutAudioStreamData(a->conv, data, (int)len);  // (handed to the player as it plays: feed_speech)
            a->audio_at = SDL_GetTicks();
            if (!a->holding) a->mood = SPEAKING;
            break;
        }
        }
        SDL_free(data);
    }
    feed_speech(a);
    // Played out, and the computer has said it is done, or sent nothing for 2 s: it makes speech far
    // faster than it plays, so its silence means the reply is over, even when its "done" ('D') never
    // came (a connection that broke while the app was behind others).
    if (speech_left(a) <= 0 && (a->reply_done || SDL_GetTicks() - a->audio_at > 2000)) {
        if (a->mood == SPEAKING) a->mood = IDLE;
        a->saying = NULL;
    }
    if (a->mood == THINKING && a->reply && a->reply_done) a->mood = IDLE;
}

// ---- Talking ----

static bool open_mic(app *a);

// A press on the character records at once, kept on the phone; only once it has been held still for
// HOLD_MS is it a hold, which stops a reply being spoken and sends what was recorded. A drag (scrolling)
// or a press let go sooner drops the recording and changes nothing.
enum { HOLD_MS = 250 };

static void begin_press(app *a) {
    if (!a->link || gp_link_status(a->link, NULL, 0) != GP_OPEN) return;
    // (opened here when it could not be at the start, before the system's question about the microphone was answered)
    if (!open_mic(a)) return;
    SDL_ClearAudioStream(a->mic);
    SDL_ResumeAudioStreamDevice(a->mic);
    a->pressing = true;
    a->held_at = SDL_GetTicks();
}

static void drop_press(app *a) {
    a->pressing = false;
    if (!a->always) SDL_PauseAudioStreamDevice(a->mic), SDL_ClearAudioStream(a->mic);
}

static void start_holding(app *a) {
    a->pressing = false;
    if (a->mood == SPEAKING) gp_link_send(a->link, 'X', NULL, 0), stop_speaking(a);
    a->holding = true;
    a->mood = LISTENING;
    a->note[0] = 0;
}

// What the microphone has: to the computer as it comes, and its level for the character.
static void send_audio(app *a) {
    Sint16 buf[1600];
    for (int got; (got = SDL_GetAudioStreamData(a->mic, buf, sizeof buf)) > 0;) {
        int n = got / 2;
        double sum = 0;
        for (int i = 0; i < n; i++) sum += (double)buf[i] * buf[i];
        float rms = (float)SDL_sqrt(sum / (n ? n : 1)) / 32768.0f;
        a->level = SDL_max(a->level * 0.7f, SDL_min(rms * 8, 1));
        gp_link_send(a->link, 'A', buf, (uint32_t)got);
    }
}

// Always-on: the microphone streams while nothing is being spoken, so the reply is not heard as the user.
static bool open_mic(app *a) {
    if (a->mic) return true;
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, MIC_RATE };
    if (!(a->mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL))) {
        SDL_snprintf(a->note, sizeof a->note, "no microphone: %s", SDL_GetError());
        return false;
    }
    return true;
}

static void always_listen(app *a) {
    bool open = a->link && gp_link_status(a->link, NULL, 0) == GP_OPEN && a->always_sent;
    // (while a reply plays too: the computer cancels its echo, by the playback reported with each piece,
    // and stops the reply when the user talks over it, as the window does)
    bool listen = a->always && open && !SDL_GetAtomicInt(&behind) && open_mic(a);
    if (!a->mic) return;
    if (listen && SDL_AudioStreamDevicePaused(a->mic)) SDL_ClearAudioStream(a->mic), SDL_ResumeAudioStreamDevice(a->mic);
    if (!listen && !a->holding && !a->pressing && !SDL_AudioStreamDevicePaused(a->mic)) SDL_PauseAudioStreamDevice(a->mic), SDL_ClearAudioStream(a->mic), a->level = 0;
    if (listen) {
        char at[48];
        bool playing = a->mood == SPEAKING && speech_left(a) > 0;
        if (playing) SDL_snprintf(at, sizeof at, "1 %.3f", gs_stream_position(a->play) - a->reply_base);
        else SDL_strlcpy(at, "0", sizeof at);
        gp_link_send(a->link, 'Q', at, (uint32_t)SDL_strlen(at));
        send_audio(a);
    }
}

static void set_always(app *a, bool on) {
    a->always = on;
    save_settings(a);
    if (a->link && gp_link_status(a->link, NULL, 0) == GP_OPEN) gp_link_send(a->link, 'O', on ? "1" : "0", 1), a->always_sent = on;
    a->left = -1;
    if (!on) a->mood = a->mood == SPEAKING ? SPEAKING : IDLE;
}

// Always-on: a tap stops the reply, or sends what has been said without waiting for the countdown.
static void tap_always(app *a) {
    if (a->mood == SPEAKING) gp_link_send(a->link, 'X', NULL, 0), stop_speaking(a), a->mood = IDLE;
    else if (a->left >= 0) gp_link_send(a->link, 'N', NULL, 0), a->left = -1;
}

static void stop_holding(app *a) {
    if (!a->holding) return;
    send_audio(a);
    SDL_PauseAudioStreamDevice(a->mic);
    a->holding = false;
    a->level = 0;
    // A tap is not speech: under half a second is dropped there too, and said here.
    if (SDL_GetTicks() - a->held_at < 400) { gp_link_send(a->link, 'X', NULL, 0); SDL_strlcpy(a->note, "hold to talk", sizeof a->note); a->mood = IDLE; return; }
    gp_link_send(a->link, 'E', NULL, 0);
    a->mood = THINKING;
}

// ---- Drawing ----

// The agent's character: a round body that breathes, blinks, listens with rings, thinks with its eyes
// up and dots going round, and talks with its mouth.
static void character(gs_ui *ui, float cx, float cy, float r, mood m, float t, float level) {
    SDL_FColor body = m == LISTENING ? ACCENT : m == SPEAKING ? OK : m == THINKING ? hex(0x8a7a62) : hex(0x9a8a72);
    float breathe = 1 + 0.025f * SDL_sinf(t * 2.2f);
    if (m == LISTENING) {
        // Rings going out, faster and wider with the voice.
        for (int i = 0; i < 3; i++) {
            float phase = SDL_fmodf(t * 1.2f + (float)i / 3, 1);
            gs_ui_opacity(ui, (1 - phase) * 0.5f);
            gs_ui_circle(ui, cx, cy, r * (1.05f + phase * (0.4f + level * 0.5f)), ACCENT);
            gs_ui_circle(ui, cx, cy, r * (1.03f + phase * (0.4f + level * 0.5f)), BACKGROUND);
        }
        gs_ui_opacity(ui, 1);
        breathe = 1 + 0.06f * level + 0.02f * SDL_sinf(t * 9);
    }
    float br = r * breathe;
    gs_ui_circle(ui, cx, cy + br * 0.06f, br, hex(0x1a1612));  // its shadow
    gs_ui_circle(ui, cx, cy, br, body);
    gs_ui_circle(ui, cx - br * 0.45f, cy - br * 0.5f, br * 0.16f, (SDL_FColor){ 1, 1, 1, 0.1f });  // a highlight
    // Eyes: looking up and aside while thinking, wide while listening, shut a moment every few seconds.
    float ex = br * 0.3f, ey = -br * 0.12f, ew = br * 0.13f, eh = br * 0.2f;
    float lookx = 0, looky = 0;
    if (m == THINKING) lookx = br * 0.08f * SDL_sinf(t * 1.3f), looky = -br * 0.08f;
    if (m == LISTENING) eh *= 1.15f;
    float blink = SDL_fmodf(t, 4.1f);
    if (blink < 0.14f) eh = br * 0.03f;
    for (int side = -1; side <= 1; side += 2) {
        gs_rect e = { cx + side * ex - ew / 2 + lookx, cy + ey - eh / 2 + looky, ew, eh };
        gs_ui_round(ui, e, ew / 2, hex(0x161210));
    }
    // The mouth: a smile at rest, an "o" listening, moving while it talks.
    float mw = br * 0.34f, mh = br * 0.07f, my = cy + br * 0.3f;
    if (m == LISTENING) mw = br * 0.16f, mh = br * (0.1f + 0.12f * level);
    if (m == SPEAKING) mh = br * (0.06f + 0.14f * (0.5f + 0.5f * SDL_sinf(t * 13) * SDL_sinf(t * 5.3f)));
    if (m == THINKING) mw = br * 0.2f;
    gs_ui_round(ui, (gs_rect){ cx - mw / 2, my - mh / 2, mw, mh }, SDL_min(mw, mh) / 2, hex(0x161210));
    if (m == THINKING) {
        for (int i = 0; i < 3; i++) {
            float a = t * 3 + (float)i * 2.094f;
            gs_ui_circle(ui, cx + SDL_cosf(a) * br * 1.25f, cy + SDL_sinf(a) * br * 1.25f, br * 0.05f, DIM);
        }
    }
}

// ---- The characters ----

// Four drawn characters (assets/avatars, generated with Codex's image tool), each four frames of the
// same size and place: at rest, blinking, speaking and listening.
static const char *const avatars[] = { "tomato", "fox", "girl", "robot" };
enum { AVATARS = 4, REST = 0, BLINK, SPEAK, LISTEN };
static SDL_Texture *frames[AVATARS][4];

static void load_avatars(SDL_Renderer *ren) {
    static const char *const states[] = { "rest", "blink", "speak", "listen" };
    for (int i = 0; i < AVATARS; i++)
        for (int k = 0; k < 4; k++) {
            char path[1100];
            SDL_snprintf(path, sizeof path, "%savatars/%s-%s.png", SDL_GetBasePath(), avatars[i], states[k]);
            SDL_Surface *img = SDL_LoadPNG(path);
            if (!img) { SDL_Log("gear: no %s: %s", path, SDL_GetError()); continue; }
            frames[i][k] = SDL_CreateTextureFromSurface(ren, img);
            SDL_SetTextureScaleMode(frames[i][k], SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(img);
        }
}

static int avatar_index(const char *name) {
    for (int i = 0; i < AVATARS; i++) if (!SDL_strcmp(avatars[i], name)) return i;
    return 0;
}

// The chosen character in its frame for the mood: listening while it listens, the mouth opening and
// closing while it speaks, blinking every few seconds otherwise; rings out from it while listening,
// dots round it while thinking, and breathing all the time. False when its frames are not there, for
// the drawn face instead.
static bool avatar(gs_ui *ui, int which, float cx, float cy, float r, mood m, float t, float level) {
    SDL_Texture **f = frames[which];
    if (!f[REST] || !f[BLINK] || !f[SPEAK] || !f[LISTEN]) return false;
    float breathe = 1 + 0.02f * SDL_sinf(t * 2.2f);
    if (m == LISTENING) {
        for (int i = 0; i < 3; i++) {
            float phase = SDL_fmodf(t * 1.2f + (float)i / 3, 1);
            gs_ui_opacity(ui, (1 - phase) * 0.45f);
            gs_ui_circle(ui, cx, cy, r * (0.95f + phase * (0.45f + level * 0.5f)), ACCENT);
            gs_ui_circle(ui, cx, cy, r * (0.93f + phase * (0.45f + level * 0.5f)), BACKGROUND);
        }
        gs_ui_opacity(ui, 1);
        breathe = 1 + 0.05f * level + 0.015f * SDL_sinf(t * 9);
    }
    bool blinking = SDL_fmodf(t, 4.1f) < 0.14f;
    int k = REST;
    if (m == LISTENING) k = LISTEN;
    else if (m == SPEAKING && SDL_sinf(t * 13) * SDL_sinf(t * 5.3f) > 0.05f) k = SPEAK;
    else if (blinking) k = BLINK;
    float bob = m == THINKING ? r * 0.03f * SDL_sinf(t * 3) : 0, size = 2 * r * breathe;
    gs_ui_texture(ui, (gs_rect){ cx - size / 2, cy - size / 2 + bob, size, size }, f[k], NULL);
    if (m == THINKING)
        for (int i = 0; i < 3; i++) {
            float ang = t * 3 + (float)i * 2.094f;
            gs_ui_circle(ui, cx + SDL_cosf(ang) * r * 1.15f, cy + SDL_sinf(ang) * r * 1.15f, r * 0.05f, DIM);
        }
    return true;
}

static const char *hint(app *a, char *buf, size_t size) {
    char why[256] = "";
    gp_link_state st = a->link ? gp_link_status(a->link, why, sizeof why) : GP_FAILED;
    if (!a->link) return "set up the computer in settings";
    if (st == GP_CONNECTING) return SDL_snprintf(buf, size, "%s…", why), buf;
    if (st == GP_FAILED) return SDL_snprintf(buf, size, "%s", why), buf;
    if (a->always) {
        if (a->mood == SPEAKING) return "speaking: talk or tap to stop";
        if (a->mood == THINKING) return a->note[0] ? a->note : "sent";
        if (a->left >= 0) return SDL_snprintf(buf, size, "sending in %.0f s: tap to send now", SDL_ceil(a->left)), buf;
        return a->note[0] ? a->note : "listening";
    }
    switch (a->mood) {
    case LISTENING: return "listening: let go to send";
    case THINKING: return a->note[0] ? a->note : "hearing…";
    case SPEAKING: return "speaking: hold to interrupt";
    default: return a->note[0] ? a->note : "hold to talk";
    }
}

static float px = 18, small = 14;
static gs_rect character_area;  // where the character was drawn, which holding down on starts listening

// The conversations to open: those with messages, the newest first, then the agents without one yet.
typedef struct { const char *who; int last; int agent; } topic;  // last: its newest message, or -1

static int agent_of(app *a, const char *who) {
    for (int i = 0; i < a->agent_count; i++)
        if (!SDL_strcmp(a->agents[i][1][0] ? a->agents[i][1] : a->agents[i][0], who)) return i;
    return -1;
}

static int topics(app *a, topic *out, int max) {
    int n = 0;
    for (int i = a->count - 1; i >= 0 && n < max; i--) {
        int k = 0;
        while (k < n && SDL_strcmp(out[k].who, a->history[i].who)) k++;
        if (k == n) out[n++] = (topic){ a->history[i].who, i, agent_of(a, a->history[i].who) };
    }
    for (int i = 0; i < a->agent_count && n < max; i++) {
        const char *who = a->agents[i][1][0] ? a->agents[i][1] : a->agents[i][0];
        int k = 0;
        while (k < n && SDL_strcmp(out[k].who, who)) k++;
        if (k == n) out[n++] = (topic){ who, -1, i };
    }
    return n;
}

// How well a conversation matches what is typed: 0 not at all, 2 at the start of its name or its
// agent's, 1 inside.
static int topic_rank(app *a, const topic *c, const char *typed) {
    const char *name = c->agent >= 0 ? a->agents[c->agent][0] : "";
    size_t n = SDL_strlen(typed);
    if (!n || !SDL_strncasecmp(c->who, typed, n) || !SDL_strncasecmp(name, typed, n)) return 2;
    return SDL_strcasestr(c->who, typed) || SDL_strcasestr(name, typed) ? 1 : 0;
}

// Where the list was drawn, and whether a tap went down and up outside it, which closes it.
static gs_rect picker_area;
static bool tapped_outside;

// The conversations, narrowed by what is typed in the field above them and scrolled with a finger: each
// with its unseen replies and its newest message. A tap opens one, Enter the first, and back or "close"
// leaves the list.
static void picker(gs_ui *ui, app *a, gs_rect area) {
    static char typed[64];
    static int selected;
    enum { MOST = HISTORY < 256 ? HISTORY : 256 };
    static topic all[MOST], shown[MOST];
    int count = topics(a, all, MOST), n = 0;
    for (int best = 2; best >= 1; best--)
        for (int i = 0; i < count; i++)
            if (topic_rank(a, &all[i], typed) == best) shown[n++] = all[i];
    gs_rect m = gs_ui_modal_begin(ui, area.w - 24, area.h * 0.85f);
    picker_area = m;
    gs_rect in = gs_inset(m, 12);
    gs_rect head = gs_cut_top(&in, px * 2);
    gs_rect close = gs_cut_right(&head, gs_ui_text_width(ui, "close", px) + 16);
    gs_ui_text(ui, close, "close", px, gs_ui_hovered(ui, close) ? ACCENT : TEXT, 1);
    gs_ui_text(ui, head, "Conversations", px, DIM, -1);
    gs_ui_field(ui, gs_cut_top(&in, px * 2.4f), "pick", typed, sizeof typed, "search conversations and agents");
    gs_cut_top(&in, 8);
    bool done = gs_ui_clicked(ui, close) || gs_ui_key(ui, SDLK_AC_BACK, 0) || gs_ui_key(ui, SDLK_ESCAPE, 0) || tapped_outside;
    // (the agents asked for again every 3 s while the list shows, for which are mid-turn)
    if (a->link && SDL_GetTicks() - a->asked_agents >= 3000 && gp_link_status(a->link, NULL, 0) == GP_OPEN)
        gp_link_send(a->link, 'L', NULL, 0), a->asked_agents = SDL_GetTicks();
    const char *chosen = NULL;
    if (!n) gs_ui_text(ui, gs_cut_top(&in, px * 2), count ? "nothing matches" : "looking for agents…", px, DIM, -1);
    else {
        float row_h = px * 1.4f + small * 1.4f + 16;
        selected = SDL_clamp(selected, 0, n - 1);
        gs_ui_rows rows = gs_ui_list(ui, in, "conversations", n, row_h, &selected);
        for (int k = rows.first; k < rows.end; k++) {
            const topic *c = &shown[k];
            gs_rect row = gs_inset((gs_rect){ in.x, rows.y + k * row_h, in.w, row_h }, 8);
            if (k == selected && rows.activated) chosen = c->who;
            int *unseen = unread_of(a, c->who, false);
            if (unseen && *unseen) {
                char count_text[16];
                SDL_snprintf(count_text, sizeof count_text, "%d", *unseen);
                float d = small * 1.5f, w = SDL_max(d, gs_ui_text_width(ui, count_text, small) + 10);
                gs_rect badge = { row.x + row.w - w, row.y + (row.h - d) / 2, w, d };
                gs_ui_round(ui, badge, d / 2, ACCENT);
                gs_ui_text(ui, badge, count_text, small, BACKGROUND, 0);
                row.w -= w + 8;
            }
            gs_rect top = gs_cut_top(&row, px * 1.4f);
            // Whether its agent is mid-turn (a dot pulsing) or done (a still one); none without an agent.
            gs_rect mark = gs_cut_left(&top, px);
            if (c->agent >= 0) {
                float cy = mark.y + mark.h / 2, cx = mark.x + px * 0.3f;
                if (a->busy[c->agent]) {
                    float pulse = 0.5f + 0.5f * SDL_sinf((float)SDL_GetTicks() / 1000.0f * 5.0f);
                    gs_ui_circle(ui, cx, cy, px * 0.3f, (SDL_FColor){ ACCENT.r, ACCENT.g, ACCENT.b, 0.25f + 0.35f * pulse });
                    gs_ui_circle(ui, cx, cy, px * 0.18f, ACCENT);
                } else gs_ui_circle(ui, cx, cy, px * 0.18f, OK);
            }
            // The conversation's name first; its agent's, dim, in what is left of the line (cut short to
            // fit, and left out with less than a few letters' room).
            float title_w = gs_ui_text(ui, top, c->who, px, !SDL_strcmp(c->who, a->open) ? ACCENT : unseen && *unseen ? BRIGHT : TEXT, -1);
            gs_rect rest = { top.x + title_w + 12, top.y, top.w - title_w - 12, top.h };
            if (c->agent >= 0 && SDL_strcmp(c->who, a->agents[c->agent][0]) && rest.w >= small * 5)
                gs_ui_text(ui, rest, a->agents[c->agent][0], small, DIM, 1);
            char preview[200];
            if (c->last >= 0) {
                const entry *e = &a->history[c->last];
                SDL_snprintf(preview, sizeof preview, "%s%s%s", e->at, e->mine ? "  you: " : "  ", e->text);
                for (char *q = preview; *q; q++)
                    if (*q == '\n') *q = ' ';
            } else SDL_strlcpy(preview, c->agent >= 0 ? "no messages yet" : "", sizeof preview);
            gs_ui_text(ui, gs_cut_top(&row, small * 1.4f), preview, small, DIM, -1);
        }
        gs_ui_list_end(ui);
        if (gs_ui_key(ui, SDLK_RETURN, 0)) chosen = shown[0].who;
    }
    if (chosen) open_conversation(a, chosen), done = true;
    if (done) a->picking = false, typed[0] = 0, selected = 0;
    gs_ui_modal_end(ui);
}

// Drawing the conversation beside the open one, as a swipe brings it in: nothing in it takes a tap.
static bool preview;

// One message as the log draws it: a bar down its left, amber for an agent and dim for the user; a line
// with a small mark (the agent's character, or a round "you"), who and when; then the text, the user's
// dimmed. Returns its height; draws nothing when `draw` is false.
static float message_block(gs_ui *ui, app *a, gs_rect at, const entry *e, bool live, bool draw) {
    float head = small * 1.7f, pad = 8, w = at.w - pad - 4;
    const char *text = e->text;
    gp_rich_style style = { e->mine ? small + 1 : px, 11, e->mine ? DIM : (live ? BRIGHT : TEXT), ACCENT, BORDER, (SDL_FColor){ 1, 1, 1, 0.04f } };
    float h = head + gp_rich(ui, (gs_rect){ at.x + pad, 0, w, 0 }, text, &style, false) + 8;
    if (!draw) return h;
    gs_ui_fill(ui, (gs_rect){ at.x, at.y, 3, h - 4 }, e->mine ? BORDER : ACCENT);
    if (!e->mine) gs_ui_fill(ui, (gs_rect){ at.x + 3, at.y, at.w - 3, h - 4 }, (SDL_FColor){ 0.77f, 0.53f, 0.17f, 0.06f });
    float mark = head - 4, x = at.x + pad;
    int which = avatar_index(a->avatar);
    if (!e->mine && frames[which][REST]) gs_ui_texture(ui, (gs_rect){ x, at.y + 2, mark, mark }, frames[which][REST], NULL);
    else {
        gs_ui_circle(ui, x + mark / 2, at.y + 2 + mark / 2, mark / 2, RAISED);
        gs_ui_text(ui, (gs_rect){ x, at.y + 2, mark, mark }, "you", small * 0.6f, DIM, 0);
    }
    char label[200];
    SDL_snprintf(label, sizeof label, "%s", e->mine ? "you" : e->who);
    gs_rect line = { x + mark + 6, at.y, w - mark - 6, head };
    gs_ui_text(ui, line, label, small, e->mine ? DIM : ACCENT, -1);
    // (what is being said now has no time yet, nor a speaker)
    bool said_now = !e->at[0];
    float time_w = gs_ui_text(ui, line, said_now ? "now" : e->at, small * 0.9f, DIM, 1);
    // To the left of the time, a speaker: three bars to speak the message, a square to stop it.
    if (!said_now) {
        float s = small * 1.05f, ix = line.x + line.w - time_w - 10 - s, iy = at.y + (head - s) / 2;
        gs_rect hit = { ix - s, at.y - 4, s * 3, head + 8 };
        bool now = a->saying == e->text;
        SDL_FColor c = now ? ACCENT : gs_ui_hovered(ui, hit) ? TEXT : DIM;
        if (now) gs_ui_round(ui, (gs_rect){ ix + s * 0.1f, iy + s * 0.1f, s * 0.8f, s * 0.8f }, s * 0.15f, c);
        else {
            static const float bars[3] = { 0.45f, 1.0f, 0.7f };
            for (int k = 0; k < 3; k++) {
                float bh = s * bars[k], bw = s * 0.2f;
                gs_ui_round(ui, (gs_rect){ ix + k * s * 0.4f, iy + (s - bh) / 2, bw, bh }, bw / 2, c);
            }
        }
        if (!preview && gs_ui_clicked(ui, hit)) say_again(a, e);
    }
    gp_rich(ui, (gs_rect){ x, at.y + head, w, 0 }, text, &style, true);
    return h;
}

// The conversation: a page a finger scrolls back through, the newest at the end; a new message scrolls
// to its start, and what is being said now (always-on's words as they come) is shown last.
static void conversation(gs_ui *ui, app *a, gs_rect r, const char *key) {
    static int last_counts[2] = { -1, -1 };
    static char last_keys[2][128];
    int *last_count = &last_counts[preview];
    const char *id = preview ? "chat-beside" : "chat";
    if (SDL_strcmp(last_keys[preview], key)) SDL_strlcpy(last_keys[preview], key, 128), *last_count = -1;
    int shown = 0;
    for (int i = 0; i < a->count; i++)
        if (!SDL_strcmp(a->history[i].who, key)) shown++;
    gs_rect page = gs_ui_page_begin(ui, r, id);
    float y = page.y, newest = page.y;
    for (int i = 0; i < a->count; i++) {
        const entry *e = &a->history[i];
        if (SDL_strcmp(e->who, key)) continue;
        bool speaking = !preview && a->saying == e->text && a->mood == SPEAKING;
        newest = y;
        y += message_block(ui, a, (gs_rect){ page.x, y, page.w, 0 }, e, speaking, true) + 8;
    }
    if (!preview && a->said && *a->said) {
        entry now = { a->said, "", "", true };
        SDL_strlcpy(now.who, here(a), sizeof now.who);
        gs_ui_opacity(ui, 0.7f);
        y += message_block(ui, a, (gs_rect){ page.x, y, page.w, 0 }, &now, true, true) + 8;
        gs_ui_opacity(ui, 1);
    }
    if (!shown && (preview || !(a->said && *a->said))) gs_ui_text(ui, (gs_rect){ page.x, y, page.w, px * 2 }, "what you say and the replies show here", small, DIM, -1);
    gs_ui_page_end(ui, y);
    // (to the newest message's start when one came, and to the end on opening)
    bool fresh = !preview && a->fresh;
    if (fresh || *last_count < 0) gs_ui_page_scroll_to(ui, id, (newest - page.y), *last_count >= 0);
    if (!preview) a->fresh = false;
    *last_count = shown;
}

// The talk screen of the conversation `key`: the open one, or with `preview` set, the one beside it.
static gs_rect compose_area;  // the box to type in, a press on which stops what is being spoken
static gs_rect chat_area;  // where the open conversation's page and the box to type in were, which a swipe does not start on

// The typed message, to the agent of the open conversation: shown when the computer sends it back ('M').
static void send_typed(gs_ui *ui, app *a) {
    char *s = a->typed, *e = s + SDL_strlen(s);
    while (*s == ' ' || *s == '\n') s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\n')) e--;
    if (s == e) return;
    if (!a->link || gp_link_status(a->link, NULL, 0) != GP_OPEN) { SDL_strlcpy(a->note, "not connected: the message waits here", sizeof a->note); return; }
    gp_link_send(a->link, 'M', s, (uint32_t)(e - s));
    a->typed[0] = 0;
    gs_ui_edited(ui, "compose", a->typed, 0);
}

static void talk_screen(gs_ui *ui, app *a, gs_rect area, float t, const char *key) {
    gs_ui_fill(ui, area, BACKGROUND);
    gs_rect r = { area.x + 8, area.y + 10, area.w - 16, area.h - 14 };
    // The top line: the agent, which a tap changes; the voice, which a tap switches; and settings.
    gs_rect top = gs_cut_top(&r, px * 2.2f);
    char why[8];
    gp_link_state st = a->link ? gp_link_status(a->link, why, sizeof why) : GP_FAILED;
    gs_ui_circle(ui, top.x + 6, top.y + top.h / 2, 5, st == GP_OPEN ? OK : st == GP_CONNECTING ? ACCENT : OVER);
    gs_rect settings = gs_cut_right(&top, gs_ui_text_width(ui, "settings", small) + 8);
    gs_ui_text(ui, settings, "settings", small, gs_ui_hovered(ui, settings) ? ACCENT : DIM, 1);
    if (!preview && gs_ui_clicked(ui, settings)) a->setup = true;
    gs_cut_right(&top, 16);
    gs_rect voice = gs_cut_right(&top, gs_ui_text_width(ui, "mary", small) + 8);
    gs_ui_text(ui, voice, a->voice[0] ? a->voice : "alba", small, gs_ui_hovered(ui, voice) ? ACCENT : DIM, 1);
    if (!preview && gs_ui_clicked(ui, voice) && st == GP_OPEN) gp_link_send(a->link, 'V', !SDL_strcmp(a->voice, "mary") ? "alba" : "mary", 4);
    gs_rect who = { top.x + 18, top.y, top.w - 18, top.h };
    // (with the count of replies unseen in the other conversations)
    int elsewhere = 0;
    for (int i = 0; i < a->unread_count; i++) elsewhere += SDL_strcmp(a->unread[i].who, key) ? a->unread[i].n : 0;
    char label[200], count_text[16] = "";
    if (elsewhere) SDL_snprintf(count_text, sizeof count_text, "%d", elsewhere);
    SDL_snprintf(label, sizeof label, "→ %s", key[0] ? key : "nobody: read back");
    float label_w = gs_ui_text(ui, who, label, px, gs_ui_hovered(ui, who) ? ACCENT : BRIGHT, -1);
    if (elsewhere) {
        float d = small * 1.5f, w = SDL_max(d, gs_ui_text_width(ui, count_text, small) + 10);
        gs_rect badge = { who.x + SDL_min(label_w, who.w) + 10, who.y + (who.h - d) / 2, w, d };
        gs_ui_round(ui, badge, d / 2, ACCENT);
        gs_ui_text(ui, badge, count_text, small, BACKGROUND, 0);
    }
    if (!preview && gs_ui_clicked(ui, who)) {
        a->picking = true;
        if (st == GP_OPEN) gp_link_send(a->link, 'L', NULL, 0);
    }
    // The box to type in, at the bottom, growing to five lines, with its send button. While it has the
    // keyboard the character and what is under it fold away, for the conversation to have the room.
    bool typing = !preview && gs_ui_focused(ui, "compose");
    {
        float send_w = gs_ui_text_width(ui, "send", px) + 24;
        float h = gs_ui_textarea_height(ui, a->typed, r.w - send_w - 8, 5) + 12;
        gs_rect box = gs_cut_bottom(&r, h);
        if (!preview) compose_area = box;
        gs_rect send = gs_cut_right(&box, send_w);
        gs_cut_right(&box, 8);
        if (preview) {
            gs_ui_outline(ui, box, 4, 1, BORDER);
            gs_ui_text(ui, gs_inset(box, 8), "type a message", small, DIM, -1);
        } else {
            gs_ui_textarea(ui, box, "compose", a->typed, sizeof a->typed, "type a message");
            bool any = a->typed[0] != 0;
            gs_ui_round(ui, send, 6, any ? ACCENT : RAISED);
            gs_ui_text(ui, send, "send", px, any ? BACKGROUND : DIM, 0);
            if ((gs_ui_clicked(ui, send) || (typing && gs_ui_key(ui, SDLK_RETURN, 0))) && any) send_typed(ui, a);
        }
        gs_cut_bottom(&r, 8);
        if (!preview) chat_area = (gs_rect){ r.x, r.y, r.w, box.y + box.h - r.y };  // (amended below, once the page's top is known)
    }
    // The character, a third of the screen at most, with what it is doing and always-on's switch under it.
    if (!typing) {
        gs_cut_top(&r, 8);
        float block = SDL_min(r.w * 0.62f, r.h * 0.3f), radius = block * 0.4f;
        gs_rect stage = gs_cut_top(&r, block);
        float cx = stage.x + stage.w / 2, cy = stage.y + stage.h / 2;
        mood shown = preview ? IDLE : a->always && a->mood == IDLE && st == GP_OPEN ? LISTENING : a->mood;
        if (!avatar(ui, avatar_index(a->avatar), cx, cy, radius * 1.25f, shown, t, a->level)) character(ui, cx, cy, radius, shown, t, a->level);
        if (!preview) character_area = (gs_rect){ cx - radius * 1.2f, cy - radius * 1.2f, radius * 2.4f, radius * 2.4f };
        char buf[300];
        bool listening = !preview && (a->mood == LISTENING || (a->always && a->mood == IDLE));
        gs_ui_text(ui, gs_cut_top(&r, px * 1.8f), preview ? "" : hint(a, buf, sizeof buf), small, listening ? ACCENT : DIM, 0);
        gs_rect toggle = gs_cut_top(&r, px * 2);
        const char *always_label = a->always ? "always on: on" : "always on: off";
        float tw = gs_ui_text_width(ui, always_label, small) + 24;
        gs_rect sw = { toggle.x + (toggle.w - tw) / 2, toggle.y, tw, toggle.h };
        gs_ui_outline(ui, sw, sw.h / 2, 1, a->always ? ACCENT : BORDER);
        gs_ui_text(ui, sw, always_label, small, a->always ? ACCENT : gs_ui_hovered(ui, sw) ? TEXT : DIM, 0);
        if (!preview && gs_ui_clicked(ui, sw)) set_always(a, !a->always);
    } else {
        char buf[300];
        character_area = (gs_rect){ 0 };
        gs_ui_text(ui, gs_cut_top(&r, px * 1.6f), hint(a, buf, sizeof buf), small, DIM, 0);
    }
    // What was said and the reply: the rest of the screen, a page a finger scrolls, back at its top for each
    // new reply so it reads from the start.
    gs_cut_top(&r, 12);
    gs_ui_fill(ui, (gs_rect){ r.x, r.y, r.w, 1 }, BORDER);
    gs_cut_top(&r, 10);
    if (!preview) chat_area = (gs_rect){ r.x, r.y, r.w, chat_area.y + chat_area.h - r.y };
    conversation(ui, a, r, key);
    gs_ui_wake(ui, 16);
    // The agents to pick from, over everything.
    if (!preview && a->picking) picker(ui, a, area);
}

// ---- Swiping between conversations ----

// A swipe outside the conversation's page, left or up, opens the conversation after the open one in the
// list; right or down, the one before. As pagers do (sources/swipe/NOTES.md): the screen follows the
// finger from when it has moved 16 points along one axis, with the conversation beside it coming in;
// let go, it goes on to that one after a fling (25 points and 400 points a second its way) or past
// half way, and back otherwise, easing out in at most 600 ms. With no conversation that way the
// screen moves less and less, as a rubber band does.
enum { SWIPE_SLOP = 16, FLING_DISTANCE = 25, FLING_SPEED = 400, SETTLE_MS = 600, SAMPLES = 8 };
typedef struct {
    bool down;               // a finger down outside the page: perhaps a swipe
    int axis;                // 0 until it has moved the slop, then 1 across or 2 up and down
    float x0, y0, size;      // where it went down; the screen's width or height along the axis
    float off;               // how far the screen is moved, the rubber band applied
    float pos[SAMPLES], at[SAMPLES];  // the latest offsets and their times, for the speed let go at
    int samples;
    bool settling;           // easing from `from` to `to` since `began`, over `ms`
    float from, to, ms;
    Uint64 began;
    char next[128];          // the conversation it lands on, when it goes on
} swipe;

// The conversation `step` from the open one in the list (1 the next, -1 the one before).
static bool beside(app *a, int step, char *out, size_t size) {
    static topic list[256];
    int n = topics(a, list, 256), at = -1;
    for (int i = 0; i < n; i++)
        if (!SDL_strcmp(list[i].who, a->open)) at = i;
    int k = at < 0 ? (step > 0 ? 0 : -1) : at + step;
    if (k < 0 || k >= n) return false;
    SDL_strlcpy(out, list[k].who, size);
    return true;
}

static float rubber(float x, float d) {
    float r = (1.0f - 1.0f / (SDL_fabsf(x) * 0.55f / d + 1.0f)) * d;
    return x < 0 ? -r : r;
}

static void swipe_down(swipe *w, float x, float y) {
    if (w->settling) return;
    *w = (swipe){ .down = true, .x0 = x, .y0 = y };
}

static void swipe_move(swipe *w, app *a, float x, float y, gs_rect area) {
    if (w->down && !w->axis && a->holding) w->down = false;  // (a finger kept still on the character is talking)
    if (!w->down) return;
    float dx = x - w->x0, dy = y - w->y0;
    if (!w->axis) {
        if (SDL_sqrtf(dx * dx + dy * dy) < SWIPE_SLOP) return;
        w->axis = SDL_fabsf(dx) >= SDL_fabsf(dy) ? 1 : 2;
        w->size = w->axis == 1 ? area.w : area.h;
    }
    float raw = w->axis == 1 ? dx : dy;
    raw -= raw < 0 ? -SWIPE_SLOP : SWIPE_SLOP;  // (from where it was taken for a swipe, so it does not jump)
    char next[128];
    w->off = beside(a, raw < 0 ? 1 : -1, next, sizeof next) ? SDL_clamp(raw, -w->size, w->size) : rubber(raw, w->size);
    if (w->samples == SAMPLES) SDL_memmove(w->pos, w->pos + 1, sizeof w->pos - sizeof *w->pos), SDL_memmove(w->at, w->at + 1, sizeof w->at - sizeof *w->at), w->samples--;
    w->pos[w->samples] = raw, w->at[w->samples] = (float)SDL_GetTicks() / 1000.0f, w->samples++;
}

static void settle(swipe *w, float to, float speed) {
    float left = SDL_fabsf(to - w->off);
    w->ms = speed > 0 ? SDL_min(SETTLE_MS, 4000.0f * left / speed) : SDL_min(SETTLE_MS, (left / w->size + 1) * 100);
    w->settling = true, w->from = w->off, w->to = to, w->began = SDL_GetTicks();
}

static void swipe_up(swipe *w, app *a) {
    bool was = w->down && w->axis;
    w->down = false;
    if (!was) return;
    // The speed over the last 0.1 s of the drag.
    float now = (float)SDL_GetTicks() / 1000.0f, speed = 0;
    int first = w->samples - 1;
    while (first > 0 && now - w->at[first - 1] <= 0.1f) first--;
    if (w->samples >= 2 && first < w->samples - 1 && w->at[w->samples - 1] > w->at[first])
        speed = (w->pos[w->samples - 1] - w->pos[first]) / (w->at[w->samples - 1] - w->at[first]);
    int step = w->off < 0 ? 1 : -1;
    bool has = beside(a, step, w->next, sizeof w->next);
    bool fling = SDL_fabsf(w->off) > FLING_DISTANCE && SDL_fabsf(speed) > FLING_SPEED && (speed < 0) == (w->off < 0);
    bool on = has && (fling || SDL_fabsf(w->off) > w->size / 2);
    settle(w, on ? -step * w->size : 0, fling ? SDL_fabsf(speed) : 0);
    if (!on) w->next[0] = 0;
}

// Each frame: the easing on, and the conversation it went to opened when it ends there.
static void swipe_step(swipe *w, app *a) {
    if (!w->settling) return;
    float t = w->ms > 0 ? SDL_min(1.0f, (float)(SDL_GetTicks() - w->began) / w->ms) : 1;
    float e = t - 1;
    w->off = w->from + (w->to - w->from) * (e * e * e * e * e + 1);
    if (t < 1) return;
    w->settling = false, w->off = 0, w->axis = 0;
    if (w->next[0]) open_conversation(a, w->next);
}

static gs_rect shifted(gs_rect r, int axis, float by) {
    if (axis == 1) r.x += by;
    else r.y += by;
    return r;
}

// ---- Pronunciation ----

// `text` spoken, in place of what is being spoken, for hearing how a spelling sounds.
static void sound_out(app *a, const char *text) {
    if (!*text || !a->link || gp_link_status(a->link, NULL, 0) != GP_OPEN) return;
    if (a->saying || a->mood == SPEAKING) gp_link_send(a->link, 'X', NULL, 0), stop_speaking(a), a->mood = IDLE;
    gp_link_send(a->link, 'U', text, (uint32_t)SDL_strlen(text));
    a->reply_done = false, a->audio_at = SDL_GetTicks();
    a->reply_base = gs_stream_position(a->play) + speech_left(a);
}

static void teach(app *a, const char *word, const char *say) {
    if (!a->link) return;
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "word", gs_jval_str(word));
    gs_jval_set(o, "say", gs_jval_str(say));
    char *json = gs_jval_write(o, false);
    gp_link_send(a->link, 'Y', json, (uint32_t)SDL_strlen(json));
    SDL_free(json), gs_jval_free(o);
}

// A small button with a play mark, to hear `text`.
static bool play_button(gs_ui *ui, gs_rect r) {
    bool hot = gs_ui_hovered(ui, r);
    gs_ui_round(ui, gs_inset(r, 2), 6, hot ? RAISED : PANEL);
    float s = r.h * 0.32f, cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    static const float bars[3] = { 0.45f, 1.0f, 0.7f };
    for (int k = 0; k < 3; k++) {
        float bh = s * 2 * bars[k], bw = s * 0.35f;
        gs_ui_round(ui, (gs_rect){ cx - s + k * s * 0.8f, cy - bh / 2, bw, bh }, bw / 2, hot ? ACCENT : TEXT);
    }
    return gs_ui_clicked(ui, r);
}

// The words the speech says wrongly, taught how to say them: a word typed, CMUdict's way to use with
// a tap, or a spelling of one's own, each to be heard before it is kept; the words kept below, a tap
// on one to change it.
static void pronunciation_panel(gs_ui *ui, app *a, gs_rect area) {
    gs_rect m = gs_ui_modal_begin(ui, area.w - 24, area.h * 0.85f);
    gs_rect in = gs_inset(m, 12);
    gs_rect head = gs_cut_top(&in, px * 2);
    gs_rect close = gs_cut_right(&head, gs_ui_text_width(ui, "close", px) + 16);
    gs_ui_text(ui, close, "close", px, gs_ui_hovered(ui, close) ? ACCENT : TEXT, 1);
    gs_ui_text(ui, head, "Pronunciation", px, DIM, -1);
    bool done = gs_ui_clicked(ui, close) || gs_ui_key(ui, SDLK_AC_BACK, 0) || gs_ui_key(ui, SDLK_ESCAPE, 0);
    float row = px * 2.4f;
    char before[64];
    SDL_strlcpy(before, a->word, sizeof before);
    gs_ui_text(ui, gs_cut_top(&in, small * 1.6f), "word", small, DIM, -1);
    gs_ui_field(ui, gs_cut_top(&in, row), "word", a->word, sizeof a->word, "proton");
    // (looked up on the computer once typing pauses)
    if (SDL_strcmp(before, a->word)) a->word_typed = SDL_GetTicks(), a->dictionary[0] = 0;
    if (*a->word && SDL_strcmp(a->looked_up, a->word) && SDL_GetTicks() - a->word_typed > 400 && a->link && gp_link_status(a->link, NULL, 0) == GP_OPEN)
        gp_link_send(a->link, 'J', a->word, (uint32_t)SDL_strlen(a->word)), SDL_strlcpy(a->looked_up, a->word, sizeof a->looked_up);
    gs_cut_top(&in, 8);
    if (*a->dictionary) {
        gs_rect line = gs_cut_top(&in, row);
        gs_rect hear = gs_cut_right(&line, row);
        gs_cut_right(&line, 6);
        gs_rect use = gs_cut_right(&line, gs_ui_text_width(ui, "use", px) + 24);
        if (play_button(ui, hear)) sound_out(a, a->dictionary);
        if (gs_ui_button(ui, use, "use")) SDL_strlcpy(a->say_as, a->dictionary, sizeof a->say_as), gs_ui_edited(ui, "say as", a->say_as, (int)SDL_strlen(a->say_as));
        char label[200];
        SDL_snprintf(label, sizeof label, "dictionary: %s", a->dictionary);
        gs_ui_text(ui, line, label, small, TEXT, -1);
    } else if (*a->word && !SDL_strcmp(a->looked_up, a->word))
        gs_ui_text(ui, gs_cut_top(&in, row), "not in the dictionary: spell it as it sounds", small, DIM, -1);
    gs_cut_top(&in, 6);
    gs_ui_text(ui, gs_cut_top(&in, small * 1.6f), "say it as", small, DIM, -1);
    if (a->say_as_came) gs_ui_edited(ui, "say as", a->say_as, (int)SDL_strlen(a->say_as)), a->say_as_came = false;
    gs_rect say = gs_cut_top(&in, row);
    gs_rect save = gs_cut_right(&say, gs_ui_text_width(ui, "keep", px) + 28);
    gs_cut_right(&say, 6);
    gs_rect hear = gs_cut_right(&say, row);
    gs_cut_right(&say, 6);
    gs_ui_field(ui, say, "say as", a->say_as, sizeof a->say_as, "Pro-tawn");
    if (play_button(ui, hear)) sound_out(a, a->say_as);
    if (gs_ui_button_primary(ui, save, "keep") && *a->word && *a->say_as) teach(a, a->word, a->say_as), a->word[0] = a->say_as[0] = a->dictionary[0] = a->looked_up[0] = 0;
    // The words kept.
    gs_cut_top(&in, 14);
    gs_ui_fill(ui, (gs_rect){ in.x, in.y, in.w, 1 }, BORDER);
    gs_cut_top(&in, 8);
    if (!a->taught_count) gs_ui_text(ui, gs_cut_top(&in, row), "no words taught yet", small, DIM, -1);
    else {
        static int selected;
        float h = px * 2.2f;
        gs_ui_rows rows = gs_ui_list(ui, in, "taught", a->taught_count, h, &selected);
        for (int k = rows.first; k < rows.end; k++) {
            gs_rect r = gs_inset((gs_rect){ in.x, rows.y + k * h, in.w, h }, 4);
            gs_rect forget = gs_cut_right(&r, h);
            gs_rect hear_it = gs_cut_right(&r, h);
            char label[300];
            SDL_snprintf(label, sizeof label, "%s  \u2192  %s", a->taught[k][0], a->taught[k][1]);
            gs_ui_text(ui, r, label, small, TEXT, -1);
            if (play_button(ui, hear_it)) sound_out(a, a->taught[k][1]);
            gs_ui_text(ui, forget, "\u00d7", px, gs_ui_hovered(ui, forget) ? OVER : DIM, 0);
            if (gs_ui_clicked(ui, forget)) teach(a, a->taught[k][0], "");
            else if (k == selected && rows.activated) {
                SDL_strlcpy(a->word, a->taught[k][0], sizeof a->word), SDL_strlcpy(a->say_as, a->taught[k][1], sizeof a->say_as);
                gs_ui_edited(ui, "word", a->word, (int)SDL_strlen(a->word)), gs_ui_edited(ui, "say as", a->say_as, (int)SDL_strlen(a->say_as));
            }
        }
        gs_ui_list_end(ui);
    }
    if (done) a->teaching = false;
    gs_ui_modal_end(ui);
}

static void setup_screen(gs_ui *ui, app *a, gs_rect area) {
    gs_ui_fill(ui, area, BACKGROUND);
    gs_rect r = gs_inset(area, 16);
    gs_rect head = gs_cut_top(&r, px * 2);
    // Back to the character, once there is a computer to talk to.
    if (a->host[0] && a->user[0]) {
        gs_rect back = gs_cut_right(&head, gs_ui_text_width(ui, "back", px) + 16);
        gs_ui_text(ui, back, "back", px, gs_ui_hovered(ui, back) ? ACCENT : TEXT, 1);
        if (gs_ui_clicked(ui, back) || (!a->teaching && gs_ui_key(ui, SDLK_AC_BACK, 0))) {
            a->setup = false;
            if (!a->link) save_settings(a), connect_link(a);
        }
    }
    gs_ui_text(ui, head, "gear: the computer it talks to", px, BRIGHT, -1);
    float row = px * 2.4f;
    gs_ui_text(ui, gs_cut_top(&r, small * 1.6f), "computer (name or address)", small, DIM, -1);
    gs_ui_field(ui, gs_cut_top(&r, row), "host", a->host, sizeof a->host, "example.org");
    gs_cut_top(&r, 6);
    gs_ui_text(ui, gs_cut_top(&r, small * 1.6f), "user there, and SSH port", small, DIM, -1);
    gs_rect two = gs_cut_top(&r, row);
    gs_rect port = gs_cut_right(&two, 90);
    gs_cut_right(&two, 8);
    gs_ui_field(ui, two, "user", a->user, sizeof a->user, "user");
    gs_ui_field(ui, port, "port", a->port, sizeof a->port, "22");
    // The character: the four side by side, a tap choosing one.
    gs_cut_top(&r, 14);
    gs_ui_text(ui, gs_cut_top(&r, small * 1.6f), "character", small, DIM, -1);
    float cell = SDL_min(r.w / AVATARS, 96);
    gs_rect choices = gs_cut_top(&r, cell);
    for (int i = 0; i < AVATARS; i++) {
        gs_rect c = { choices.x + (float)i * cell, choices.y, cell, cell };
        bool chosen = !SDL_strcmp(a->avatar, avatars[i]);
        if (chosen || gs_ui_hovered(ui, c)) gs_ui_round(ui, gs_inset(c, 2), 10, chosen ? RAISED : PANEL);
        if (chosen) gs_ui_outline(ui, gs_inset(c, 2), 10, 1, ACCENT);
        if (frames[i][REST]) gs_ui_texture(ui, gs_inset(c, 8), frames[i][REST], NULL);
        else gs_ui_text(ui, c, avatars[i], small, TEXT, 0);
        if (gs_ui_clicked(ui, c)) SDL_strlcpy(a->avatar, avatars[i], sizeof a->avatar), save_settings(a);
    }
    // Words said wrongly, taught on the computer.
    gs_cut_top(&r, 14);
    if (gs_ui_button(ui, gs_cut_top(&r, row), "pronunciation: words said wrongly") && a->link) {
        a->teaching = true;
        gp_link_send(a->link, 'Y', "", 0);
    }
    gs_cut_top(&r, 14);
    gs_ui_text(ui, gs_cut_top(&r, small * 1.6f), "this phone's key, for ~/.ssh/authorized_keys there:", small, DIM, -1);
    float kh = gs_ui_text_height(ui, a->key_line, small, r.w);
    gs_ui_text_wrapped(ui, gs_cut_top(&r, kh), a->key_line, small, TEXT);
    gs_cut_top(&r, 8);
    gs_rect buttons = gs_cut_top(&r, row);
    gs_rect copy = gs_cut_left(&buttons, buttons.w / 2 - 4);
    gs_cut_left(&buttons, 8);
    if (gs_ui_button(ui, copy, "copy the key")) gs_clipboard_set(a->key_line);
    if (gs_ui_button_primary(ui, buttons, "connect")) {
        save_settings(a);
        connect_link(a);
        a->setup = false;
    }
    gs_cut_top(&r, 14);
    if (a->host_key[0]) {
        char pin[200];
        SDL_snprintf(pin, sizeof pin, "the computer's key, pinned: SHA256:%s", a->host_key);
        gs_ui_text_wrapped(ui, gs_cut_top(&r, gs_ui_text_height(ui, pin, small, r.w)), pin, small, DIM);
        gs_cut_top(&r, 6);
        if (gs_ui_button(ui, gs_cut_top(&r, row), "forget the computer's key")) a->host_key[0] = 0, save_settings(a);
    }
    char why[256];
    if (a->link && gp_link_status(a->link, why, sizeof why) == GP_FAILED) {
        gs_cut_top(&r, 10);
        gs_ui_text_wrapped(ui, r, why, small, OVER);
    }
    if (a->teaching) pronunciation_panel(ui, a, area);
}

// `gear-phone --say FILE.wav [HOST USER]`: the app's connection without its window, for trying the
// server from here. It sends the recording (16 kHz mono) as held, and prints what comes back.
static int say_file(app *a, const char *wav) {
    SDL_AudioSpec spec;
    Uint8 *data;
    Uint32 len;
    if (!SDL_LoadWAV(wav, &spec, &data, &len) || spec.format != SDL_AUDIO_S16 || spec.channels != 1 || spec.freq != MIC_RATE) {
        fprintf(stderr, "need a 16 kHz mono 16-bit WAV\n");
        return 1;
    }
    printf("this phone's key: %s\n", a->key_line);
    connect_link(a);
    char why[256], type, *got;
    uint32_t n;
    for (gp_link_state st; (st = gp_link_status(a->link, why, sizeof why)) != GP_OPEN; SDL_Delay(20))
        if (st == GP_FAILED) { printf("failed: %s\n", why); return 1; }
    Uint64 t0 = SDL_GetTicks();
    for (Uint32 at = 0; at < len; at += 3200) gp_link_send(a->link, 'A', data + at, SDL_min(3200, len - at));
    gp_link_send(a->link, 'E', NULL, 0);
    double speech = 0;
    for (bool done = false; !done;) {
        if (gp_link_status(a->link, why, sizeof why) == GP_FAILED) { printf("failed: %s\n", why); return 1; }
        while (gp_link_take(a->link, &type, &got, &n)) {
            if (type == 'P') speech += n / 2.0 / SPEECH_RATE;
            else printf("%.2f s: %c %s\n", (SDL_GetTicks() - t0) / 1000.0, type, got);
            if (type == 'D' || type == 'F') done = true;
            SDL_free(got);
        }
        SDL_Delay(10);
    }
    printf("%.2f s of speech back; the computer's key SHA256:%s\n", speech, gp_link_host_key(a->link, why, sizeof why) ? why : "?");
    gp_link_close(a->link);
    return 0;
}

// An event's window coordinates in the interface's points, as gs_ui reads them: through the window's
// pixels, then gs_ui's pixels per point.
static void window_points(SDL_Window *win, gs_ui *ui, float x, float y, float *px_out, float *py_out) {
    int w, h, pw, ph;
    SDL_GetWindowSize(win, &w, &h);
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    float to_pixels = w > 0 ? (float)pw / (float)w : 1, per_point = gs_ui_scale(ui);  // (its zoom is in its scale)
    *px_out = x * to_pixels / per_point, *py_out = y * to_pixels / per_point;
}

int main(int argc, char **argv) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait");
    // `gear-phone --shot FILE setup|talk|pick|table|swipe|type|speaking|pronounce`: a screen drawn to a picture, without a display.
    const char *shot = argc >= 4 && !SDL_strcmp(argv[1], "--shot") ? argv[2] : NULL;
    if (shot) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen"), SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    // Paused by Android (behind other apps, the phone locked), the app runs on, so a reply or message
    // being spoken plays to its end; it draws nothing until it is back (in the loop below).
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;
    static app a;
    char *pref = SDL_GetPrefPath("wakamex", "gear-phone");
    SDL_strlcpy(a.dir, pref ? pref : "", sizeof a.dir);
    SDL_free(pref);
    load_settings(&a);
    bool keyed = load_key(&a);
    if (!shot) {
        load_history(&a), load_unread(&a);
        char path[1100];
        settings_path(&a, path, sizeof path, "seen.txt");
        char *seen = SDL_LoadFile(path, NULL);
        a.seen = seen ? SDL_strtoll(seen, NULL, 10) : 0;
        SDL_free(seen);
    }
    if (shot) {
        SDL_strlcpy(a.target, "voice", sizeof a.target), SDL_strlcpy(a.voice, "alba", sizeof a.voice);
        SDL_strlcpy(a.tab, "voice", sizeof a.tab), SDL_strlcpy(a.open, "voice", sizeof a.open);
        remember(&a, "Did the gesso pin land?", "gesso", "08:30", true);
        remember(&a, "Yes, d895f7f is pushed with link finding and styled spans.", "gesso", "08:31", false);
        remember(&a, "The Kijiji search found two 3090s under 900 dollars near you.", "personal", "11:02", false);
        remember(&a, "One of them is listed as tested with a receipt.", "personal", "11:03", false);
        *unread_of(&a, "personal", true) = 2;
        remember(&a, "The production model drifts into English on long recordings. The figure is the share of common short words that are English:\n\n"
                     "| Recording | Language | Minutes | Production (`parakeet-ft-v1`) | Parakeet v3 |\n|---|---|---|---|---|\n"
                     "| Interview with an ultrarunner | German | 38.6 | 23% | 15% |\n| Comedy highlights | German | 42.9 | 52% | 18% |\n"
                     "| Comedy set | German | 12.8 | 63% | 19% |\n| Guided meditation | Italian | 11.2 | 4% | 4% |\n"
                     "| Talk on tarot | Italian | 44.7 | 31% | 4% |\n| Video on earning with AI videos | Portuguese | 31.0 | 9% | 0% |\n"
                     "| Exam strategy | Portuguese | 15.4 | 8% | 1% |\n\n- German shares words like \"in\", \"so\", \"was\" and \"will\" with English, so a clean German transcript scores about 15% here.",
                 "transcribe", "12:53", false);
        if (!SDL_strcmp(argv[3], "table")) SDL_strlcpy(a.open, "transcribe", sizeof a.open);
        remember(&a, "What did the agents get done this morning?", "voice", "09:12", true);
        remember(&a, "Three things. The transcribe agent fixed the Spanish drift, gear's phone app got a searchable agent picker, and the fellows site is up.", "voice", "09:12", false);
        remember(&a, "Can you check whether the build finished?", "voice", "10:40", true);
        a.reply = NULL;
        remember(&a, "The build finished at 4:12 and all 31 tests passed. I published it and restarted gear on your PC.\n\nThe transcribe agent found that the production model drifts into English on every long Spanish and French recording it tested, and switching back to base Parakeet fixes it. Trading3's walk-forward test came back positive for large non-vault accounts but failed for vaults.\n\nYou also started prep for something with nine days left, with the progcomp agent planning around Python basics. The fellows agent built a study site, and gear's Android app got a scrollable agent picker with search.", "voice", "10:41", false);
        if (!SDL_strcmp(argv[3], "pick")) {
            static const char *names[][2] = { { "voice", "voice" }, { "personal_claude", "personal" }, { "gear_claude", "gear" }, { "gesso", "gesso" },
                                              { "hearsay_claude", "hearsay" }, { "hyperliquid_participant_feed_re_claude", "re-opus" }, { "aeroplan_claude", "aeroplan" } };
            for (int i = 0; i < 7; i++) SDL_strlcpy(a.agents[i][0], names[i][0], 128), SDL_strlcpy(a.agents[i][1], names[i][1], 128);
            a.agent_count = 7, a.picking = true, a.busy[2] = a.busy[4] = true;
        }
    }
    if (argc >= 3 && !SDL_strcmp(argv[1], "--say")) {
        if (argc >= 5) SDL_strlcpy(a.host, argv[3], sizeof a.host), SDL_strlcpy(a.user, argv[4], sizeof a.user);
        return keyed ? say_file(&a, argv[2]) : 1;
    }
#ifdef SDL_PLATFORM_ANDROID
    SDL_RequestAndroidPermission("android.permission.RECORD_AUDIO", NULL, NULL);
    // The microphone as for a call (AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION): with the phone's own echo
    // cancelling, before the computer's.
    SDL_SetHint(SDL_HINT_ANDROID_AAUDIO_INPUT_PRESET, "7");
#endif
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "gear");
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 400);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 760);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_Window *win = SDL_CreateWindowWithProperties(p);
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    SDL_SetRenderVSync(ren, 1);
    gs_fontset *fonts = gs_fontset_system();
    char font[1100];
    SDL_snprintf(font, sizeof font, "%sClankerMono-Regular.ttf", SDL_GetBasePath());
    gs_fontset_prepend(fonts, font);
    gs_ui *ui = gs_ui_new(win, ren, fonts);
    load_avatars(ren);
    who_lock = SDL_CreateMutex();
    if (!shot) gs_notify_init("gear", "org.wakamex.gear", NULL, notification_clicked);

    gs_mix_open(MIX_RATE);
    a.play = gs_stream_new(MIX_RATE, 8.0, 0.08);
    gs_mix_add(gs_stream_render, a.play);
    const SDL_AudioSpec speech = { SDL_AUDIO_S16, 1, SPEECH_RATE }, mix = { SDL_AUDIO_F32, 2, MIX_RATE };
    a.conv = SDL_CreateAudioStream(&speech, &mix);
    const SDL_AudioSpec mic = { SDL_AUDIO_S16, 1, MIC_RATE };
    a.mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &mic, NULL, NULL);
    if (a.mic) SDL_PauseAudioStreamDevice(a.mic);

    a.setup = shot ? !SDL_strcmp(argv[3], "setup") || !SDL_strcmp(argv[3], "pronounce") : !a.host[0] || !a.user[0];
    if (shot && !SDL_strcmp(argv[3], "pronounce")) {
        a.teaching = true, a.taught_count = 2;
        SDL_strlcpy(a.word, "lepton", sizeof a.word), SDL_strlcpy(a.looked_up, "lepton", sizeof a.looked_up);
        SDL_strlcpy(a.dictionary, "lehp-tahn", sizeof a.dictionary), SDL_strlcpy(a.say_as, "Lep-tawn", sizeof a.say_as);
        SDL_strlcpy(a.taught[0][0], "proton", 128), SDL_strlcpy(a.taught[0][1], "Pro-tawn", 128);
        SDL_strlcpy(a.taught[1][0], "meson", 128), SDL_strlcpy(a.taught[1][1], "May-zawn", 128);
    }
    if (!keyed) SDL_strlcpy(a.key_line, "could not make a key for this phone", sizeof a.key_line);
    if (!a.setup && !shot) connect_link(&a);
    int frames = 0;
    Uint64 start = SDL_GetTicks();
    // Where the finger (or mouse) went down, in the interface's points, and whether it has moved since.
    float press_x = 0, press_y = 0;
    bool down = false, went_down = false, went_up = false, moved = false;
    static swipe sw;
    gs_rect last_area = { 0, 0, 400, 760 };
    if (shot && !SDL_strcmp(argv[3], "speaking"))  // (the newest reply of the open conversation being spoken)
        for (int i = a.count - 1; i >= 0; i--)
            if (!a.history[i].mine && !SDL_strcmp(a.history[i].who, a.open)) { a.saying = a.history[i].text, a.mood = SPEAKING; break; }
    if (shot && !SDL_strcmp(argv[3], "type")) SDL_strlcpy(a.typed, "Can you check whether the phone build passed, and if it did, install it on the PC too?", sizeof a.typed), gs_ui_focus(ui, "compose");
    if (shot && !SDL_strcmp(argv[3], "swipe")) sw.axis = 1, sw.size = 400, sw.off = -170;  // (40% of the way to the next)
    for (bool running = true; running;) {
        SDL_Event e;
        int wait = a.holding || a.mood != IDLE || sw.settling || sw.axis ? 5 : 30;
        if (SDL_GetAtomicInt(&behind)) wait = speech_left(&a) > 0 || a.saying ? 20 : 500;
        if (SDL_WaitEventTimeout(&e, wait)) {
            do {
                if (e.type == SDL_EVENT_QUIT) running = false;
                // Leaving the app ends what is being said; coming back, the connection is made again.
                if (e.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
                    stop_holding(&a);
                    SDL_SetAtomicInt(&behind, 1);
                    if (a.link) gp_link_set_background(a.link, true);
                }
                if (e.type == SDL_EVENT_WILL_ENTER_FOREGROUND) {
                    SDL_SetAtomicInt(&behind, 0);
                    if (a.link) gp_link_set_background(a.link, false);
                }
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                    window_points(win, ui, e.button.x, e.button.y, &press_x, &press_y);
                    down = went_down = true, moved = false;
                    if (!a.setup && !a.picking && !gs_rect_contains(chat_area, press_x, press_y)) swipe_down(&sw, press_x, press_y);
                }
                if (e.type == SDL_EVENT_MOUSE_MOTION && down) {
                    float x, y;
                    window_points(win, ui, e.motion.x, e.motion.y, &x, &y);
                    if ((x - press_x) * (x - press_x) + (y - press_y) * (y - press_y) > 12 * 12) moved = true;
                    swipe_move(&sw, &a, x, y, last_area);
                }
                if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) down = false, went_up = true, swipe_up(&sw, &a);
                gs_ui_event(ui, &e);
            } while (SDL_PollEvent(&e));
        }
        take_frames(&a);
        // A notification tapped opens its conversation.
        char opened[128];
        SDL_LockMutex(who_lock);
        SDL_strlcpy(opened, tapped, sizeof opened), tapped[0] = 0;
        SDL_UnlockMutex(who_lock);
        if (*opened) open_conversation(&a, opened);
        if (a.holding) send_audio(&a);
        else always_listen(&a);
        // Android's back leaves the app from the character, and goes back from settings and the agents.
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, a.setup || a.picking || a.teaching ? "1" : "0");
        // The computer's key, pinned once it is first seen.
        char seen[128];
        if (a.link && !a.host_key[0] && gp_link_status(a.link, NULL, 0) == GP_OPEN && gp_link_host_key(a.link, seen, sizeof seen))
            SDL_strlcpy(a.host_key, seen, sizeof a.host_key), save_settings(&a);

        // Behind other apps, or with the phone locked, the loop goes on (the speech playing) without drawing.
        if (SDL_GetAtomicInt(&behind)) continue;
        gs_rect area = gs_ui_begin(ui);
        // The whole width: in portrait what the safe area keeps clear at the sides (the back gesture's
        // edges, the screen's rounded corners) covers nothing a line of text minds.
        int out_w, out_h;
        if (SDL_GetCurrentRenderOutputSize(ren, &out_w, &out_h) && out_w < out_h) area.x = 0, area.w = (float)out_w / gs_ui_scale(ui);
        float t = (float)(SDL_GetTicks() - start) / 1000.0f;
        last_area = area;
        swipe_step(&sw, &a);
        // (by where the list was last frame: the tap that opened it does not close it)
        bool was_picking = a.picking;
        // A press in the box to type in stops what is being spoken, as typing over it would.
        if (went_down && !a.setup && !a.picking && gs_rect_contains(compose_area, press_x, press_y) && (a.saying || a.mood == SPEAKING) && a.link)
            gp_link_send(a.link, 'X', NULL, 0), stop_speaking(&a), a.mood = IDLE;
        tapped_outside = a.picking && went_up && !moved && !gs_rect_contains(picker_area, press_x, press_y);
        if (a.setup) setup_screen(ui, &a, area);
        else {
            // Mid-swipe, the conversation beside the open one comes in beside it.
            char next[128];
            int step = sw.off < 0 ? 1 : -1;
            if (sw.off != 0 && beside(&a, step, next, sizeof next)) {
                preview = true;
                talk_screen(ui, &a, shifted(area, sw.axis, sw.off + step * sw.size), t, next);
                preview = false;
            }
            talk_screen(ui, &a, sw.off != 0 ? shifted(area, sw.axis, sw.off) : area, t, a.open);
            // A press is on the character only where it went down, not where the pointer was last.
            bool on_character = went_down && !a.picking && gs_rect_contains(character_area, press_x, press_y);
            if (a.always) {
                // (a tap, let go where it went down: a finger that moves is swiping)
                if (went_up && !moved && !was_picking && !a.picking && gs_rect_contains(character_area, press_x, press_y)) tap_always(&a);
            } else {
                if (on_character && !a.holding) begin_press(&a);
                if (a.pressing && (moved || !down)) drop_press(&a);
                else if (a.pressing && SDL_GetTicks() - a.held_at >= HOLD_MS) start_holding(&a);
                if (!down && a.holding) stop_holding(&a);
            }
            went_down = went_up = false;
        }
        gs_ui_end(ui);
        if (shot && ++frames >= 4) { gs_draw_save_png(ren, shot); running = false; }
        SDL_RenderPresent(ren);
    }
    stop_holding(&a);
    gp_link_close(a.link);
    gs_mix_close();
    gs_ui_free(ui);
    SDL_Quit();
    return 0;
}
