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
    bool holding;
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
    }
    SDL_free(text);
}

static void save_settings(app *a) {
    char path[1100], text[700];
    settings_path(a, path, sizeof path, "settings.txt");
    SDL_snprintf(text, sizeof text, "host=%s\nuser=%s\nport=%s\nhost_key=%s\nalways=%d\n", a->host, a->user, a->port, a->host_key, a->always);
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
        case 'R': set_text(&a->reply, data), a->reply_done = false, a->note[0] = 0; break;
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

static void start_holding(app *a) {
    if (!a->link || gp_link_status(a->link, NULL, 0) != GP_OPEN) return;
    // (opened here when it could not be at the start, before the system's question about the microphone was answered)
    if (!open_mic(a)) return;
    if (a->mood == SPEAKING) gp_link_send(a->link, 'X', NULL, 0), stop_speaking(a);
    SDL_ClearAudioStream(a->mic);
    SDL_ResumeAudioStreamDevice(a->mic);
    a->holding = true;
    a->held_at = SDL_GetTicks();
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
    bool listen = a->always && open && a->mood != SPEAKING && open_mic(a);
    if (!a->mic) return;
    if (listen && SDL_AudioStreamDevicePaused(a->mic)) SDL_ClearAudioStream(a->mic), SDL_ResumeAudioStreamDevice(a->mic);
    if (!listen && !a->holding && !SDL_AudioStreamDevicePaused(a->mic)) SDL_PauseAudioStreamDevice(a->mic), SDL_ClearAudioStream(a->mic), a->level = 0;
    if (listen) send_audio(a);
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

static const char *hint(app *a, char *buf, size_t size) {
    char why[256] = "";
    gp_link_state st = a->link ? gp_link_status(a->link, why, sizeof why) : GP_FAILED;
    if (!a->link) return "set up the computer in settings";
    if (st == GP_CONNECTING) return SDL_snprintf(buf, size, "%s…", why), buf;
    if (st == GP_FAILED) return SDL_snprintf(buf, size, "%s", why), buf;
    if (a->always) {
        if (a->mood == SPEAKING) return "speaking: tap to stop";
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

static void talk_screen(gs_ui *ui, app *a, gs_rect area, float t) {
    gs_ui_fill(ui, area, BACKGROUND);
    gs_rect r = gs_inset(area, 14);
    // The agent at the top, which a tap changes; the voice at the right, which a tap switches.
    gs_rect top = gs_cut_top(&r, px * 2.2f);
    char why[8];
    gp_link_state st = a->link ? gp_link_status(a->link, why, sizeof why) : GP_FAILED;
    gs_ui_circle(ui, top.x + 6, top.y + top.h / 2, 5, st == GP_OPEN ? OK : st == GP_CONNECTING ? ACCENT : OVER);
    gs_rect voice = gs_cut_right(&top, gs_ui_text_width(ui, "voice: alba", small) + 16);
    char label[160];
    SDL_snprintf(label, sizeof label, "voice: %s", a->voice[0] ? a->voice : "alba");
    gs_ui_text(ui, voice, label, small, gs_ui_hovered(ui, voice) ? ACCENT : DIM, 1);
    if (gs_ui_clicked(ui, voice) && st == GP_OPEN) gp_link_send(a->link, 'V', !SDL_strcmp(a->voice, "mary") ? "alba" : "mary", 4);
    gs_rect who = { top.x + 18, top.y, top.w - 18, top.h };
    SDL_snprintf(label, sizeof label, "→ %s", a->target[0] ? a->target : "nobody: read back");
    gs_ui_text(ui, who, label, px, gs_ui_hovered(ui, who) ? ACCENT : BRIGHT, -1);
    if (gs_ui_clicked(ui, who) && st == GP_OPEN) a->picking = true, a->agent_count = 0, gp_link_send(a->link, 'L', NULL, 0);
    // Settings at the bottom.
    gs_rect bottom = gs_cut_bottom(&r, small * 2.4f);
    gs_rect gear_link = gs_cut_right(&bottom, gs_ui_text_width(ui, "settings", small) + 8);
    gs_ui_text(ui, gear_link, "settings", small, gs_ui_hovered(ui, gear_link) ? ACCENT : DIM, 1);
    if (gs_ui_clicked(ui, gear_link)) a->setup = true;
    // What was said and the reply, under the character.
    float chat_h = r.h * 0.38f;
    gs_rect chat = gs_cut_bottom(&r, chat_h);
    gs_ui_clip(ui, &chat);
    float y = chat.y;
    if (a->said) {
        y += gs_ui_text_wrapped(ui, (gs_rect){ chat.x, y, chat.w, 100000 }, a->said, small, DIM) + 10;
    }
    if (a->reply) {
        float h = gs_ui_text_height(ui, a->reply, px, chat.w);
        float room = chat.y + chat.h - y;
        gs_ui_text_wrapped(ui, (gs_rect){ chat.x, h > room ? chat.y + chat.h - h : y, chat.w, h }, a->reply, px, a->mood == SPEAKING ? BRIGHT : TEXT);
    }
    gs_ui_clip(ui, NULL);
    // The character, in the middle of what is left, and what it is doing under it.
    // Always-on's switch, under what the character is doing.
    gs_rect toggle = gs_cut_bottom(&r, px * 2.2f);
    const char *always_label = a->always ? "always on: on" : "always on: off";
    float tw = gs_ui_text_width(ui, always_label, small) + 24;
    gs_rect sw = { toggle.x + (toggle.w - tw) / 2, toggle.y, tw, toggle.h };
    gs_ui_outline(ui, sw, sw.h / 2, 1, a->always ? ACCENT : BORDER);
    gs_ui_text(ui, sw, always_label, small, a->always ? ACCENT : gs_ui_hovered(ui, sw) ? TEXT : DIM, 0);
    if (gs_ui_clicked(ui, sw)) set_always(a, !a->always);
    gs_rect line = gs_cut_bottom(&r, px * 2);
    char buf[300];
    bool listening = a->mood == LISTENING || (a->always && a->mood == IDLE);
    gs_ui_text(ui, line, hint(a, buf, sizeof buf), small, listening ? ACCENT : DIM, 0);
    float radius = SDL_min(r.w, r.h) * 0.3f;
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    character(ui, cx, cy, radius, a->always && a->mood == IDLE && st == GP_OPEN ? LISTENING : a->mood, t, a->level);
    character_area = (gs_rect){ cx - radius * 1.2f, cy - radius * 1.2f, radius * 2.4f, radius * 2.4f };
    gs_ui_wake(ui, 16);
    // The agents to pick from, over everything.
    if (a->picking) {
        gs_rect m = gs_ui_modal_begin(ui, area.w - 40, area.h * 0.7f);
        gs_rect in = gs_inset(m, 12);
        gs_ui_text(ui, gs_cut_top(&in, small * 1.8f), "Send to", small, DIM, -1);
        if (!a->agent_count) gs_ui_text(ui, gs_cut_top(&in, px * 2), "looking for agents…", px, DIM, -1);
        float row = px * 2.2f;
        for (int i = 0; i < a->agent_count && in.h >= row; i++) {
            gs_rect at = gs_cut_top(&in, row);
            const char *tab = a->agents[i][1][0] ? a->agents[i][1] : a->agents[i][0];
            if (gs_ui_hovered(ui, at)) gs_ui_fill(ui, at, RAISED);
            gs_ui_text(ui, gs_inset(at, 6), tab, px, !SDL_strcmp(a->agents[i][0], a->target) ? ACCENT : TEXT, -1);
            if (gs_ui_clicked(ui, at)) gp_link_send(a->link, 'T', a->agents[i][0], (uint32_t)SDL_strlen(a->agents[i][0])), a->picking = false;
        }
        if (gs_ui_key(ui, SDLK_ESCAPE, 0) || gs_ui_key(ui, SDLK_AC_BACK, 0)) a->picking = false;
        gs_ui_modal_end(ui);
    }
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

int main(int argc, char **argv) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;
    static app a;
    char *pref = SDL_GetPrefPath("wakamex", "gear-phone");
    SDL_strlcpy(a.dir, pref ? pref : "", sizeof a.dir);
    SDL_free(pref);
    load_settings(&a);
    bool keyed = load_key(&a);
    // `gear-phone --shot FILE setup|talk`: a screen drawn to a picture, without a display.
    const char *shot = argc >= 4 && !SDL_strcmp(argv[1], "--shot") ? argv[2] : NULL;
    if (shot) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen"), SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        SDL_strlcpy(a.target, "voice", sizeof a.target), SDL_strlcpy(a.voice, "alba", sizeof a.voice);
        a.said = SDL_strdup("Can you check whether the build finished?");
        a.reply = SDL_strdup("The build finished at 4:12 and all 31 tests passed. I published it and restarted gear on your PC.");
    }
    if (argc >= 3 && !SDL_strcmp(argv[1], "--say")) {
        if (argc >= 5) SDL_strlcpy(a.host, argv[3], sizeof a.host), SDL_strlcpy(a.user, argv[4], sizeof a.user);
        return keyed ? say_file(&a, argv[2]) : 1;
    }
#ifdef SDL_PLATFORM_ANDROID
    SDL_RequestAndroidPermission("android.permission.RECORD_AUDIO", NULL, NULL);
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
    bool was_down = false;
    for (bool running = true; running;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, a.holding || a.mood != IDLE ? 5 : 30)) {
            do {
                if (e.type == SDL_EVENT_QUIT) running = false;
                // Leaving the app ends what is being said; coming back, the connection is made again.
                if (e.type == SDL_EVENT_DID_ENTER_BACKGROUND) stop_holding(&a);
                if (e.type == SDL_EVENT_WILL_ENTER_FOREGROUND && a.link && gp_link_status(a.link, NULL, 0) == GP_FAILED) connect_link(&a);
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
            // Held on the character: a press that starts on it listens until it is let go, wherever.
            bool down = (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) != 0;
            if (a.always) {
                if (!a.picking && down && !was_down && gs_ui_hovered(ui, character_area)) tap_always(&a);
            } else if (!a.picking && down && !a.holding && !was_down && gs_ui_hovered(ui, character_area)) start_holding(&a);
            if (!down && a.holding) stop_holding(&a);
            was_down = down;
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
