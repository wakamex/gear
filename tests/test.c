// gear's tests. The listener and WAV tests always run; the end-to-end tests load the models from
// GEAR_MODELS (see the README) and are skipped without it.
#include <SDL3/SDL.h>
#include <stdio.h>
#include "gs_mix.h"
#include "gs_voice.h"
#include "agent.h"
#include "hear.h"
#include "listen.h"
#include "models.h"
#include "relay.h"
#include "pipeline.h"
#include "say.h"
#include "wav.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// Frames numbered in their first sample, fed with the given speech probabilities.
static gear_listen_event feed(gear_listen *l, int index, float speech) {
    float frame[GEAR_FRAME] = { (float)index };
    return gear_listen_frame(l, frame, speech);
}

static void test_listen(void) {
    gear_listen_params p = gear_listen_defaults();
    gear_listen *l = gear_listen_new(&p);
    int i = 0;
    for (; i < 100; i++) CHECK(feed(l, i, 0.1f) == GEAR_LISTEN_NONE);
    // A piece starts on the third frame of speech and opens with the preroll before it.
    CHECK(feed(l, i++, 0.9f) == GEAR_LISTEN_NONE);
    CHECK(feed(l, i++, 0.9f) == GEAR_LISTEN_NONE);
    CHECK(feed(l, i++, 0.9f) == GEAR_LISTEN_START);
    CHECK(gear_listen_in_piece(l));
    // A pause shorter than pause_frames does not end it; one as long does.
    for (int k = 0; k < p.pause_frames - 1; k++) CHECK(feed(l, i++, 0.1f) == GEAR_LISTEN_NONE);
    CHECK(feed(l, i++, 0.9f) == GEAR_LISTEN_NONE);
    gear_listen_event e = GEAR_LISTEN_NONE;
    for (int k = 0; k < p.pause_frames && e == GEAR_LISTEN_NONE; k++) e = feed(l, i++, 0.1f);
    CHECK(e == GEAR_LISTEN_END);
    int n;
    const float *piece = gear_listen_piece(l, &n);
    // Preroll (30 frames ending with the start frame, 102) through the last frame fed.
    CHECK(n % GEAR_FRAME == 0);
    CHECK(piece[0] == (float)(102 - p.preroll_frames));
    CHECK(piece[n - GEAR_FRAME] == (float)(i - 1));
    CHECK(!gear_listen_in_piece(l));
    // A piece that never pauses is cut at max_frames.
    for (int k = 0; k < 3; k++) feed(l, i++, 0.9f);
    e = GEAR_LISTEN_NONE;
    int frames = 0;
    while (e != GEAR_LISTEN_END && frames < p.max_frames + 10) { e = feed(l, i++, 0.9f); frames++; }
    CHECK(e == GEAR_LISTEN_END);
    gear_listen_piece(l, &n);
    CHECK(n == p.max_frames * GEAR_FRAME);
    gear_listen_free(l);
}

static void test_wav(const char *tmp) {
    float tone[1600];
    for (int i = 0; i < 1600; i++) tone[i] = 0.5f * SDL_sinf(2 * SDL_PI_F * 440 * (float)i / 16000);
    char path[1100];
    SDL_snprintf(path, sizeof path, "%stone.wav", tmp);
    CHECK(gear_wav_write(path, tone, 1600, 1, 16000));
    int n;
    float *back = gear_wav_read(path, 16000, &n);
    CHECK(back && n == 1600);
    float worst = 0;
    for (int i = 0; back && i < n; i++) worst = SDL_max(worst, SDL_fabsf(back[i] - tone[i]));
    CHECK(worst < 1e-4f);
    SDL_free(back);
}

// A LibriSpeech utterance (CC-BY-4.0, test-clean 1089-134686-0010), padded with silence.
static const char *utterance_text = "well now ennis i declare you have a head and so has my stick";

static void lower_letters(char *s) {
    char *o = s;
    for (; *s; s++) if (SDL_isalpha(*s) || *s == ' ') *o++ = (char)SDL_tolower(*s);
    *o = 0;
}

enum room { HEADPHONES, LOUDSPEAKERS, LOUDSPEAKERS_NO_CANCELLER };

// A reply plays through the mix, rendered offline in real time, while `speech` plays as the
// microphone two seconds in. With loudspeakers the microphone also hears the reply, at half its level
// and 100 ms late. Reports when the reply was first heard, when talking stopped it and when the player
// went quiet, in seconds, -1 for what did not happen.
static bool duplex(const char *model, const char *voice_path, gear_hear *hear, const float *speech, int n, enum room room, double *heard, double *interrupted, double *quiet) {
    char error[512];
    gs_voice *voice = gs_voice_new(GEAR_RATE, GS_VOICE_SPEECH | (room == LOUDSPEAKERS ? GS_VOICE_ECHO : 0));
    gear_say *say = gear_say_open(model, voice_path, 2, room == LOUDSPEAKERS ? voice : NULL, error, sizeof error);
    CHECK(say);
    if (!say) { fprintf(stderr, "%s\n", error); gs_voice_free(voice); return false; }
    gear_listen_params lp = gear_listen_defaults();
    gear_pipeline *p = gear_pipeline_new(voice, hear, say, &lp, 1.0);
    gear_say_speak(say, "Here is the plan. First, I will split the reply into sentences. Then each sentence goes to the speech engine while the previous one is playing.");
    enum { STEP = 480, DELAY = 10 };
    float lr[STEP * 2], frame[GEAR_FRAME], echo[DELAY][GEAR_FRAME] = { { 0 } };
    *heard = *interrupted = *quiet = -1;
    Uint64 start = SDL_GetTicksNS();
    for (int step = 0; step < 600 && *quiet < 0; step++) {
        gs_mix_render(lr, STEP);
        double level = 0;
        for (int i = 0; i < STEP * 2; i++) level = SDL_max(level, SDL_fabs(lr[i]));
        double t = (step + 1) / 100.0;
        if (*heard < 0 && level > 1e-3) *heard = t;
        if (*interrupted >= 0 && *quiet < 0 && level < 1e-4) *quiet = t;
        int mic = step * GEAR_FRAME - 2 * GEAR_RATE;
        SDL_memset(frame, 0, sizeof frame);
        if (mic >= 0 && mic + GEAR_FRAME <= n) SDL_memcpy(frame, speech + mic, sizeof frame);
        if (room != HEADPHONES) {
            // What played 100 ms ago reaches the microphone: 48 kHz stereo averaged to 16 kHz mono.
            float *late = echo[step % DELAY];
            for (int i = 0; i < GEAR_FRAME; i++) frame[i] += 0.5f * late[i];
            for (int i = 0; i < GEAR_FRAME; i++) late[i] = (lr[6 * i] + lr[6 * i + 1] + lr[6 * i + 2] + lr[6 * i + 3] + lr[6 * i + 4] + lr[6 * i + 5]) / 6;
        }
        int before = gear_pipeline_interruptions(p);
        gear_pipeline_frame(p, frame);
        if (gear_pipeline_interruptions(p) > before && *interrupted < 0) *interrupted = t;
        SDL_free(gear_pipeline_poll(p));
        Uint64 due = start + (Uint64)(step + 1) * 10000000;
        if (SDL_GetTicksNS() < due) SDL_DelayNS(due - SDL_GetTicksNS());
    }
    static const char *names[] = { "headphones", "loudspeakers with the canceller", "loudspeakers without it" };
    printf("%s: first sound %.2f s, talking heard %.2f s, quiet %.2f s\n", names[room], *heard, *interrupted, *quiet);
    gear_pipeline_free(p);
    gear_say_close(say);
    gs_voice_free(voice);
    return true;
}

static void test_end_to_end(const char *dir, const char *data) {
    char path[1200], wav[1200], error[512];
    SDL_snprintf(wav, sizeof wav, "%s/utterance.wav", data);
    int n;
    float *speech = gear_wav_read(wav, GEAR_RATE, &n);
    CHECK(speech);
    if (!speech) return;

    gear_model_path(dir, GEAR_MODEL_HEAR, path, sizeof path);
    gear_hear *hear = gear_hear_open(path, 1, error, sizeof error);
    CHECK(hear);
    if (!hear) { fprintf(stderr, "%s\n", error); return; }

    // Hearing: the utterance through the pipeline makes one message, sent after the countdown.
    gs_voice *voice = gs_voice_new(GEAR_RATE, GS_VOICE_SPEECH);
    gear_listen_params lp = gear_listen_defaults();
    gear_pipeline *p = gear_pipeline_new(voice, hear, NULL, &lp, 1.0);
    char *message = NULL;
    float frame[GEAR_FRAME];
    for (int i = 0; !message && i < n + GEAR_RATE * 10; i += GEAR_FRAME) {
        SDL_memset(frame, 0, sizeof frame);
        if (i + GEAR_FRAME <= n) SDL_memcpy(frame, speech + i, sizeof frame);
        while (gear_hear_pending(hear)) SDL_Delay(1);  // as live, where transcription outpaces speech
        gear_pipeline_frame(p, frame);
        message = gear_pipeline_poll(p);
    }
    CHECK(message);
    if (message) {
        lower_letters(message);
        if (SDL_strcmp(message, utterance_text)) fprintf(stderr, "heard: %s\n", message);
        CHECK(!SDL_strcmp(message, utterance_text));
    }
    SDL_free(message);
    gear_pipeline_free(p);

    // Saying and interrupting, with headphones (the microphone hears only the user), then with
    // loudspeakers (it also hears the reply) and the echo canceller, and as a control loudspeakers
    // without it, where the reply should cut itself off.
    char model[1200], voice_path[1200];
    gear_model_path(dir, GEAR_MODEL_SAY, model, sizeof model);
    gear_model_path(dir, GEAR_MODEL_VOICE, voice_path, sizeof voice_path);
    gs_voice_free(voice);
    double heard, interrupted, quiet;
    if (duplex(model, voice_path, hear, speech, n, HEADPHONES, &heard, &interrupted, &quiet)) {
        CHECK(heard > 0 && heard < 1.0);  // the reply starts within a second
        // The utterance's speech begins 0.56 s into the file, so 2.56 s into the test; it is heard
        // within 0.15 s of that, and the reading stops at once.
        CHECK(interrupted >= 2.56 && interrupted < 2.71);
        CHECK(quiet >= interrupted && quiet - interrupted < 0.05);
    }
    if (gs_voice_has_echo()) {
        if (duplex(model, voice_path, hear, speech, n, LOUDSPEAKERS, &heard, &interrupted, &quiet))
            CHECK(interrupted >= 2.56 && interrupted < 2.71);  // the reply's echo is not taken for talking
        if (duplex(model, voice_path, hear, speech, n, LOUDSPEAKERS_NO_CANCELLER, &heard, &interrupted, &quiet))
            CHECK(interrupted > 0 && interrupted < 2.56);  // without the canceller the reply stops itself
    } else printf("echo tests skipped: build with -Decho=true\n");
    gear_hear_close(hear);
    SDL_free(speech);
}

// Against the real wakterm, with an agent that does not exist, so nothing is delivered: the refusal
// (which wakterm reports with exit code 0) must come back as a failure with its reason.
static void test_agent_refused(void) {
    char error[512], id[37];
    gear_agent nobody = { .name = "nobody", .id = "gear-test-no-such-agent", .incarnation = "none" };
    bool sent = gear_agent_send(&nobody, "test", id, error, sizeof error);
    if (SDL_strstr(error, "Could not run")) { printf("agent test skipped: %s\n", error); return; }
    CHECK(!sent);
    CHECK(SDL_strstr(error, "not available") != NULL);
    gear_agent found;
    CHECK(!gear_agent_find("gear-test-no-such-agent", &found, error, sizeof error));
}

// A real round trip, when GEAR_AGENT names an agent kept for tests: a message out, its reply back.
static void test_agent_round_trip(const char *name) {
    char error[512];
    gear_agent a;
    CHECK(gear_agent_find(name, &a, error, sizeof error));
    if (!a.id[0]) { fprintf(stderr, "%s\n", error); return; }
    gear_relay *r = gear_relay_new(&a);
    gear_relay_send(r, "Reply with exactly the word ready.");
    char *reply = NULL;
    bool failed = false;
    Uint64 deadline = SDL_GetTicks() + 120000;
    while (!(reply = gear_relay_take(r, &failed, error, sizeof error)) && !failed && SDL_GetTicks() < deadline) SDL_Delay(100);
    gear_relay_free(r);
    if (failed) fprintf(stderr, "%s\n", error);
    CHECK(reply && SDL_strcasestr(reply, "ready"));
    printf("%s replied: %s\n", name, reply ? reply : "(nothing)");
    SDL_free(reply);
}

int main(int argc, char **argv) {
    const char *data = argc > 1 ? argv[1] : "tests";
    SDL_Init(SDL_INIT_AUDIO);
    char *tmp = SDL_GetPrefPath("wakamex", "gear-tests");
    test_listen();
    test_wav(tmp);
    test_agent_refused();
    const char *agent = SDL_getenv("GEAR_AGENT");
    if (agent && *agent) test_agent_round_trip(agent);
    else printf("agent round trip skipped: GEAR_AGENT is not set\n");
    const char *dir = SDL_getenv("GEAR_MODELS");
    if (dir && gear_models_present(dir)) test_end_to_end(dir, data);
    else printf("end-to-end tests skipped: GEAR_MODELS does not hold the models\n");
    SDL_free(tmp);
    SDL_Quit();
    if (failures) fprintf(stderr, "%d failed\n", failures);
    else printf("all passed\n");
    return failures ? 1 : 0;
}
