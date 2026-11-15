// gear on a phone: a character to hold while talking. Held, it listens and what is said goes to gear on
// the computer over SSH; let go, it is sent to the agent, and the agent's reply is spoken back. The
// computer does the hearing, the agent and the speech (src/serve.c); the phone records, plays and draws.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include "gs_draw.h"
#include "gs_json.h"
#include "gs_mix.h"
#include "gs_secret.h"
#include "gs_stream.h"
#include "gs_text.h"
#include "gs_ui.h"
#include "key.h"
#include "link.h"

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

typedef struct {
    // Where gear runs, kept in settings.txt; the phone's key, kept with gs_secret.
    char host[256], user[64], port[8], host_key[128];
    bool always;          // always on: listening without holding, the computer finding the messages
    char avatar[16];      // the character drawn: fox, tomato, girl or robot
    char pem[1024], key_line[400];
    char dir[1024];
    gp_link *link;
    Uint64 retry_at;
    bool setup;           // the settings screen is showing
    char status[300];     // the connection's state, or why it failed
    // The conversation.
    char target[128], voice[16], note[300];
    char *said, *reply;
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
    double reply_base;    // where the reply being played starts in the player's count of seconds
    // The agents to pick from.
    bool picking;
    char agents[MAX_AGENTS][2][128];  // name, and the tab's title
    int agent_count;
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
    }
    SDL_free(text);
}

static void save_settings(app *a) {
    char path[1100], text[760];
    settings_path(a, path, sizeof path, "settings.txt");
    SDL_snprintf(text, sizeof text, "host=%s\nuser=%s\nport=%s\nhost_key=%s\nalways=%d\navatar=%s\n", a->host, a->user, a->port, a->host_key, a->always, a->avatar);
    SDL_SaveFile(path, text, SDL_strlen(text));
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
    a->link = gp_link_open(&c);
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
        a->agent_count++;
    }
    gs_json_free(doc);
}

static void stop_speaking(app *a) {
    gs_stream_flush(a->play);
    SDL_ClearAudioStream(a->conv);
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
                SDL_strlcpy(a->voice, gs_json_str(gs_json_get(gs_json_root(doc), "voice"), "alba"), sizeof a->voice);
            }
            gs_json_free(doc);
            if (a->always && !a->always_sent) gp_link_send(a->link, 'O', "1", 1), a->always_sent = true;
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
        case 'M': set_text(&a->said, data), a->note[0] = 0, a->left = -1; if (a->always) a->mood = THINKING; break;
        case 'R':
            set_text(&a->reply, data), a->reply_done = false, a->note[0] = 0;
            a->reply_base = gs_stream_position(a->play) + gs_stream_buffered(a->play);
            break;
        case 'I': stop_speaking(a), a->mood = IDLE; break;  // talked over: the computer stopped it
        case 'S': SDL_strlcpy(a->note, data, sizeof a->note); break;
        case 'F': SDL_strlcpy(a->note, data, sizeof a->note), a->mood = IDLE; break;
        case 'G': parse_agents(a, data); break;
        case 'D': a->reply_done = true; break;
        case 'P': {
            int n = (int)len / 2;
            float *f = SDL_malloc((size_t)n * sizeof *f);
            for (int i = 0; i < n; i++) f[i] = (float)(Sint16)((Uint8)data[2 * i] | (Uint8)data[2 * i + 1] << 8) / 32768.0f;
            SDL_PutAudioStreamData(a->conv, f, n * (int)sizeof *f);
            SDL_free(f);
            float buf[4096];
            for (int got; (got = SDL_GetAudioStreamData(a->conv, buf, sizeof buf)) > 0;) gs_stream_write(a->play, buf, got / (int)(2 * sizeof(float)));
            if (!a->holding) a->mood = SPEAKING;
            break;
        }
        }
        SDL_free(data);
    }
    if (a->mood == SPEAKING && a->reply_done && gs_stream_buffered(a->play) <= 0) a->mood = IDLE;
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
    bool listen = a->always && open && open_mic(a);
    if (!a->mic) return;
    if (listen && SDL_AudioStreamDevicePaused(a->mic)) SDL_ClearAudioStream(a->mic), SDL_ResumeAudioStreamDevice(a->mic);
    if (!listen && !a->holding && !a->pressing && !SDL_AudioStreamDevicePaused(a->mic)) SDL_PauseAudioStreamDevice(a->mic), SDL_ClearAudioStream(a->mic), a->level = 0;
    if (listen) {
        char at[48];
        bool playing = a->mood == SPEAKING && gs_stream_buffered(a->play) > 0;
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

// How well agent i matches what is typed: 0 not at all, higher better; with nothing typed, the agent
// sent to now first.
static int pick_rank(app *a, int i, const char *typed) {
    const char *name = a->agents[i][0], *tab = a->agents[i][1];
    if (!*typed) return !SDL_strcmp(name, a->target) ? 2 : 1;
    size_t n = SDL_strlen(typed);
    if (!SDL_strncasecmp(tab, typed, n) || !SDL_strncasecmp(name, typed, n)) return 3;
    return SDL_strcasestr(tab, typed) || SDL_strcasestr(name, typed) ? 2 : 0;
}

static const char *agent_title(app *a, int i) { return a->agents[i][1][0] ? a->agents[i][1] : a->agents[i][0]; }

// The agents, narrowed by what is typed in the field above them and scrolled with a finger; a tap on
// one sends to it from now on, Enter takes the first, and back or "close" leaves it.
static void picker(gs_ui *ui, app *a, gs_rect area) {
    static char typed[64];
    static int selected;
    int order[MAX_AGENTS], rank[MAX_AGENTS], n = 0;
    for (int i = 0; i < a->agent_count; i++) {
        int r = pick_rank(a, i, typed);
        if (!r) continue;
        int at = n++;
        for (; at > 0 && (rank[at - 1] < r || (rank[at - 1] == r && SDL_strcasecmp(agent_title(a, order[at - 1]), agent_title(a, i)) > 0)); at--)
            order[at] = order[at - 1], rank[at] = rank[at - 1];
        order[at] = i, rank[at] = r;
    }
    gs_rect m = gs_ui_modal_begin(ui, area.w - 24, area.h * 0.8f);
    gs_rect in = gs_inset(m, 12);
    gs_rect head = gs_cut_top(&in, px * 2);
    gs_rect close = gs_cut_right(&head, gs_ui_text_width(ui, "close", px) + 16);
    gs_ui_text(ui, close, "close", px, gs_ui_hovered(ui, close) ? ACCENT : TEXT, 1);
    gs_ui_text(ui, head, "Send to", px, DIM, -1);
    gs_ui_field(ui, gs_cut_top(&in, px * 2.4f), "pick", typed, sizeof typed, "search tabs and agents");
    gs_cut_top(&in, 8);
    bool done = gs_ui_clicked(ui, close) || gs_ui_key(ui, SDLK_AC_BACK, 0) || gs_ui_key(ui, SDLK_ESCAPE, 0);
    const char *chosen = NULL;
    if (!a->agent_count) gs_ui_text(ui, gs_cut_top(&in, px * 2), "looking for agents…", px, DIM, -1);
    else if (!n) gs_ui_text(ui, gs_cut_top(&in, px * 2), "no agent matches", px, DIM, -1);
    else {
        float row_h = px * 2.4f;
        selected = SDL_clamp(selected, 0, n - 1);
        gs_ui_rows rows = gs_ui_list(ui, in, "agents", n, row_h, &selected);
        for (int k = rows.first; k < rows.end; k++) {
            int i = order[k];
            gs_rect row = { in.x, rows.y + k * row_h, in.w, row_h };
            if (k == selected && rows.activated) chosen = a->agents[i][0];
            bool now = !SDL_strcmp(a->agents[i][0], a->target);
            gs_rect text = gs_inset(row, 8);
            gs_ui_text(ui, text, agent_title(a, i), px, now ? ACCENT : TEXT, -1);
            if (SDL_strcmp(agent_title(a, i), a->agents[i][0])) gs_ui_text(ui, text, a->agents[i][0], small, DIM, 1);
        }
        gs_ui_list_end(ui);
        if (gs_ui_key(ui, SDLK_RETURN, 0)) chosen = a->agents[order[0]][0];
    }
    if (chosen) gp_link_send(a->link, 'T', chosen, (uint32_t)SDL_strlen(chosen)), done = true;
    if (done) a->picking = false, typed[0] = 0, selected = 0;
    gs_ui_modal_end(ui);
}

static void talk_screen(gs_ui *ui, app *a, gs_rect area, float t) {
    gs_ui_fill(ui, area, BACKGROUND);
    gs_rect r = gs_inset(area, 14);
    // The top line: the agent, which a tap changes; the voice, which a tap switches; and settings.
    gs_rect top = gs_cut_top(&r, px * 2.2f);
    char why[8];
    gp_link_state st = a->link ? gp_link_status(a->link, why, sizeof why) : GP_FAILED;
    gs_ui_circle(ui, top.x + 6, top.y + top.h / 2, 5, st == GP_OPEN ? OK : st == GP_CONNECTING ? ACCENT : OVER);
    gs_rect settings = gs_cut_right(&top, gs_ui_text_width(ui, "settings", small) + 8);
    gs_ui_text(ui, settings, "settings", small, gs_ui_hovered(ui, settings) ? ACCENT : DIM, 1);
    if (gs_ui_clicked(ui, settings)) a->setup = true;
    gs_cut_right(&top, 16);
    gs_rect voice = gs_cut_right(&top, gs_ui_text_width(ui, "mary", small) + 8);
    gs_ui_text(ui, voice, a->voice[0] ? a->voice : "alba", small, gs_ui_hovered(ui, voice) ? ACCENT : DIM, 1);
    if (gs_ui_clicked(ui, voice) && st == GP_OPEN) gp_link_send(a->link, 'V', !SDL_strcmp(a->voice, "mary") ? "alba" : "mary", 4);
    gs_rect who = { top.x + 18, top.y, top.w - 18, top.h };
    char label[160];
    SDL_snprintf(label, sizeof label, "→ %s", a->target[0] ? a->target : "nobody: read back");
    gs_ui_text(ui, who, label, px, gs_ui_hovered(ui, who) ? ACCENT : BRIGHT, -1);
    if (gs_ui_clicked(ui, who) && st == GP_OPEN) a->picking = true, a->agent_count = 0, gp_link_send(a->link, 'L', NULL, 0);
    // The character, a third of the screen at most, with what it is doing and always-on's switch under it.
    gs_cut_top(&r, 8);
    float block = SDL_min(r.w * 0.62f, r.h * 0.3f), radius = block * 0.4f;
    gs_rect stage = gs_cut_top(&r, block);
    float cx = stage.x + stage.w / 2, cy = stage.y + stage.h / 2;
    mood shown = a->always && a->mood == IDLE && st == GP_OPEN ? LISTENING : a->mood;
    if (!avatar(ui, avatar_index(a->avatar), cx, cy, radius * 1.25f, shown, t, a->level)) character(ui, cx, cy, radius, shown, t, a->level);
    character_area = (gs_rect){ cx - radius * 1.2f, cy - radius * 1.2f, radius * 2.4f, radius * 2.4f };
    char buf[300];
    bool listening = a->mood == LISTENING || (a->always && a->mood == IDLE);
    gs_ui_text(ui, gs_cut_top(&r, px * 1.8f), hint(a, buf, sizeof buf), small, listening ? ACCENT : DIM, 0);
    gs_rect toggle = gs_cut_top(&r, px * 2);
    const char *always_label = a->always ? "always on: on" : "always on: off";
    float tw = gs_ui_text_width(ui, always_label, small) + 24;
    gs_rect sw = { toggle.x + (toggle.w - tw) / 2, toggle.y, tw, toggle.h };
    gs_ui_outline(ui, sw, sw.h / 2, 1, a->always ? ACCENT : BORDER);
    gs_ui_text(ui, sw, always_label, small, a->always ? ACCENT : gs_ui_hovered(ui, sw) ? TEXT : DIM, 0);
    if (gs_ui_clicked(ui, sw)) set_always(a, !a->always);
    // What was said and the reply: the rest of the screen, a page a finger scrolls, back at its top for each
    // new reply so it reads from the start.
    gs_cut_top(&r, 12);
    gs_ui_fill(ui, (gs_rect){ r.x, r.y, r.w, 1 }, BORDER);
    gs_cut_top(&r, 10);
    static const char *shown_reply;
    if (a->reply != shown_reply) shown_reply = a->reply, gs_ui_page_scroll_to(ui, "chat", 0, false);
    gs_rect page = gs_ui_page_begin(ui, r, "chat");
    float y = page.y;
    if (a->said) y += gs_ui_text_wrapped(ui, (gs_rect){ page.x, y, page.w, 100000 }, a->said, small, DIM) + 12;
    if (a->reply) y += gs_ui_text_wrapped(ui, (gs_rect){ page.x, y, page.w, 100000 }, a->reply, px, a->mood == SPEAKING ? BRIGHT : TEXT) + 12;
    gs_ui_page_end(ui, y);
    gs_ui_wake(ui, 16);
    // The agents to pick from, over everything.
    if (a->picking) picker(ui, a, area);
}

static void setup_screen(gs_ui *ui, app *a, gs_rect area) {
    gs_ui_fill(ui, area, BACKGROUND);
    gs_rect r = gs_inset(area, 16);
    gs_rect head = gs_cut_top(&r, px * 2);
    // Back to the character, once there is a computer to talk to.
    if (a->host[0] && a->user[0]) {
        gs_rect back = gs_cut_right(&head, gs_ui_text_width(ui, "back", px) + 16);
        gs_ui_text(ui, back, "back", px, gs_ui_hovered(ui, back) ? ACCENT : TEXT, 1);
        if (gs_ui_clicked(ui, back) || gs_ui_key(ui, SDLK_AC_BACK, 0)) {
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;
    static app a;
    char *pref = SDL_GetPrefPath("wakamex", "gear-phone");
    SDL_strlcpy(a.dir, pref ? pref : "", sizeof a.dir);
    SDL_free(pref);
    load_settings(&a);
    bool keyed = load_key(&a);
    // `gear-phone --shot FILE setup|talk|pick`: a screen drawn to a picture, without a display.
    const char *shot = argc >= 4 && !SDL_strcmp(argv[1], "--shot") ? argv[2] : NULL;
    if (shot) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen"), SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        SDL_strlcpy(a.target, "voice", sizeof a.target), SDL_strlcpy(a.voice, "alba", sizeof a.voice);
        a.said = SDL_strdup("Can you check whether the build finished?");
        a.reply = SDL_strdup("The build finished at 4:12 and all 31 tests passed. I published it and restarted gear on your PC.\n\nThe transcribe agent found that the production model drifts into English on every long Spanish and French recording it tested, and switching back to base Parakeet fixes it. Trading3's walk-forward test came back positive for large non-vault accounts but failed for vaults.\n\nYou also started prep for something with nine days left, with the progcomp agent planning around Python basics. The fellows agent built a study site, and gear's Android app got a scrollable agent picker with search.");
        if (!SDL_strcmp(argv[3], "pick")) {
            static const char *names[][2] = { { "voice", "voice" }, { "personal_claude", "personal" }, { "gear_claude", "gear" }, { "gesso", "gesso" },
                                              { "hearsay_claude", "hearsay" }, { "gink_claude", "gink" }, { "aeroplan_claude", "aeroplan" } };
            for (int i = 0; i < 7; i++) SDL_strlcpy(a.agents[i][0], names[i][0], 128), SDL_strlcpy(a.agents[i][1], names[i][1], 128);
            a.agent_count = 7, a.picking = true;
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

    gs_mix_open(MIX_RATE);
    a.play = gs_stream_new(MIX_RATE, 300.0, 0.08);
    gs_mix_add(gs_stream_render, a.play);
    const SDL_AudioSpec speech = { SDL_AUDIO_F32, 1, SPEECH_RATE }, mix = { SDL_AUDIO_F32, 2, MIX_RATE };
    a.conv = SDL_CreateAudioStream(&speech, &mix);
    const SDL_AudioSpec mic = { SDL_AUDIO_S16, 1, MIC_RATE };
    a.mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &mic, NULL, NULL);
    if (a.mic) SDL_PauseAudioStreamDevice(a.mic);

    a.setup = shot ? !SDL_strcmp(argv[3], "setup") : !a.host[0] || !a.user[0];
    if (!keyed) SDL_strlcpy(a.key_line, "could not make a key for this phone", sizeof a.key_line);
    if (!a.setup && !shot) connect_link(&a);
    int frames = 0;
    Uint64 start = SDL_GetTicks();
    // Where the finger (or mouse) went down, in the interface's points, and whether it has moved since.
    float press_x = 0, press_y = 0;
    bool down = false, went_down = false, moved = false;
    for (bool running = true; running;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, a.holding || a.mood != IDLE ? 5 : 30)) {
            do {
                if (e.type == SDL_EVENT_QUIT) running = false;
                // Leaving the app ends what is being said; coming back, the connection is made again.
                if (e.type == SDL_EVENT_DID_ENTER_BACKGROUND) stop_holding(&a);
                if (e.type == SDL_EVENT_WILL_ENTER_FOREGROUND && a.link && gp_link_status(a.link, NULL, 0) == GP_FAILED) connect_link(&a);
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                    window_points(win, ui, e.button.x, e.button.y, &press_x, &press_y);
                    down = went_down = true, moved = false;
                }
                if (e.type == SDL_EVENT_MOUSE_MOTION && down) {
                    float x, y;
                    window_points(win, ui, e.motion.x, e.motion.y, &x, &y);
                    if ((x - press_x) * (x - press_x) + (y - press_y) * (y - press_y) > 12 * 12) moved = true;
                }
                if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) down = false;
                gs_ui_event(ui, &e);
            } while (SDL_PollEvent(&e));
        }
        take_frames(&a);
        if (a.holding) send_audio(&a);
        else always_listen(&a);
        // Android's back leaves the app from the character, and goes back from settings and the agents.
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, a.setup || a.picking ? "1" : "0");
        // A connection that failed is made again every five seconds, unless the key was refused or changed.
        char why[256];
        if (a.link && !a.setup && gp_link_status(a.link, why, sizeof why) == GP_FAILED && !SDL_strstr(why, "key")) {
            if (!a.retry_at) a.retry_at = SDL_GetTicks() + 5000;
            else if (SDL_GetTicks() >= a.retry_at) a.retry_at = 0, connect_link(&a);
        }
        // The computer's key, pinned once it is first seen.
        char seen[128];
        if (a.link && !a.host_key[0] && gp_link_status(a.link, NULL, 0) == GP_OPEN && gp_link_host_key(a.link, seen, sizeof seen))
            SDL_strlcpy(a.host_key, seen, sizeof a.host_key), save_settings(&a);

        gs_rect area = gs_ui_begin(ui);
        float t = (float)(SDL_GetTicks() - start) / 1000.0f;
        if (a.setup) setup_screen(ui, &a, area);
        else {
            talk_screen(ui, &a, area, t);
            // A press is on the character only where it went down, not where the pointer was last.
            bool on_character = went_down && !a.picking && gs_rect_contains(character_area, press_x, press_y);
            if (a.always) {
                if (on_character) tap_always(&a);
            } else {
                if (on_character && !a.holding) begin_press(&a);
                if (a.pressing && (moved || !down)) drop_press(&a);
                else if (a.pressing && SDL_GetTicks() - a.held_at >= HOLD_MS) start_holding(&a);
                if (!down && a.holding) stop_holding(&a);
            }
            went_down = false;
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
