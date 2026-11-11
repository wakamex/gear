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
#include "speakable.h"
#include "speech.h"
#include "transcript.h"
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
    // A piece starts on the start_frames-th frame of speech in a row and opens with the preroll before it.
    for (int k = 1; k < p.start_frames; k++) CHECK(feed(l, i++, 0.9f) == GEAR_LISTEN_NONE);
    int start = i;
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
    // Preroll (the frames ending with the start frame) through the last frame fed.
    CHECK(n % GEAR_FRAME == 0);
    CHECK(piece[0] == (float)(start - p.preroll_frames));
    CHECK(piece[n - GEAR_FRAME] == (float)(i - 1));
    CHECK(!gear_listen_in_piece(l));
    // A piece that never pauses is cut at max_frames.
    for (int k = 0; k < p.start_frames; k++) feed(l, i++, 0.9f);
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

enum room { HEADPHONES, LOUDSPEAKERS, LOUDSPEAKERS_NO_CANCELLER, LOUDSPEAKERS_LET_THROUGH };

// A reply plays through the mix, rendered offline in real time, while `speech` plays as the
// microphone two seconds in. With loudspeakers the microphone also hears the reply, at half its level
// and 100 ms late; LOUDSPEAKERS_LET_THROUGH is a canceller that is told what plays and takes none of it
// out. Reports when the reply was first heard, when talking stopped it and when the player went quiet,
// in seconds, -1 for what did not happen; and with `message`, the message made in the 4 s after that
// (SDL_free it), or NULL.
static bool duplex(const char *model, const char *voice_path, const char *vad_path, gear_hear *hear, const float *speech, int n, enum room room, double *heard, double *interrupted, double *quiet, char **message) {
    char error[512];
    if (message) *message = NULL;
    gs_voice *voice = gs_voice_new(GEAR_RATE, GS_VOICE_SPEECH | (room == LOUDSPEAKERS ? GS_VOICE_ECHO : 0));
    gear_say *say = gear_say_open(model, voice_path, 2, room == LOUDSPEAKERS || room == LOUDSPEAKERS_LET_THROUGH ? voice : NULL, error, sizeof error);
    CHECK(say);
    if (!say) { fprintf(stderr, "%s\n", error); gs_voice_free(voice); return false; }
    gear_vad *vad = gear_vad_open(vad_path, error, sizeof error);
    CHECK(vad);
    if (!vad) { fprintf(stderr, "%s\n", error); gear_say_close(say); gs_voice_free(voice); return false; }
    gear_listen_params lp = gear_listen_defaults();
    gear_pipeline *p = gear_pipeline_new(voice, vad, hear, say, &lp, 1.0);
    gear_say_speak(say, "Here is the plan. First, I will split the reply into sentences. Then each sentence goes to the speech engine while the previous one is playing.");
    enum { STEP = 480, DELAY = 10 };
    float lr[STEP * 2], frame[GEAR_FRAME], echo[DELAY][GEAR_FRAME] = { { 0 } };
    *heard = *interrupted = *quiet = -1;
    Uint64 start = SDL_GetTicksNS();
    for (int step = 0, after = message ? 400 : 0; step < 600 && (*quiet < 0 || after-- > 0); step++) {
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
        gear_pipeline_frame(p, frame, NULL);
        if (gear_pipeline_interruptions(p) > before && *interrupted < 0) *interrupted = t;
        char *sent = gear_pipeline_poll(p);
        if (message && !*message) *message = sent;
        else SDL_free(sent);
        Uint64 due = start + (Uint64)(step + 1) * 10000000;
        if (SDL_GetTicksNS() < due) SDL_DelayNS(due - SDL_GetTicksNS());
    }
    static const char *names[] = { "headphones", "loudspeakers with the canceller", "loudspeakers without it", "loudspeakers with a canceller that lets the reply through" };
    printf("%s: first sound %.2f s, talking heard %.2f s, quiet %.2f s", names[room], *heard, *interrupted, *quiet);
    if (message) printf(*message ? ", message \"%s\"" : ", no message", *message);
    printf("\n");
    gear_pipeline_free(p);
    gear_vad_free(vad);
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
    char vad_path[1200];
    gear_model_path(dir, GEAR_MODEL_VAD, vad_path, sizeof vad_path);
    gear_vad *vad = gear_vad_open(vad_path, error, sizeof error);
    CHECK(vad);
    if (!vad) { fprintf(stderr, "%s\n", error); gs_voice_free(voice); gear_hear_close(hear); return; }
    gear_listen_params lp = gear_listen_defaults();
    gear_pipeline *p = gear_pipeline_new(voice, vad, hear, NULL, &lp, 1.0);
    char keep[1200], *tmp = SDL_GetPrefPath("wakamex", "gear-tests");
    SDL_snprintf(keep, sizeof keep, "%sheard-%llu", tmp, (unsigned long long)SDL_GetTicksNS());
    SDL_free(tmp);
    gear_pipeline_keep(p, keep, "test-model");
    char *message = NULL;
    float frame[GEAR_FRAME];
    for (int i = 0; !message && i < n + GEAR_RATE * 10; i += GEAR_FRAME) {
        SDL_memset(frame, 0, sizeof frame);
        if (i + GEAR_FRAME <= n) SDL_memcpy(frame, speech + i, sizeof frame);
        while (gear_hear_pending(hear)) SDL_Delay(1);  // as live, where transcription outpaces speech
        gear_pipeline_frame(p, frame, NULL);
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
    // With drafts, played in real time: words show while the utterance is being said, the first of
    // them as the message has it, and the draft is gone once the piece's own text is in.
    p = gear_pipeline_new(voice, vad, hear, NULL, &lp, 1.0);
    gear_pipeline_set_drafts(p, true);
    message = NULL;
    char last[256] = "";
    int updates = 0;
    Uint64 start = SDL_GetTicksNS();
    for (int i = 0, step = 0; !message && i < n + GEAR_RATE * 10; i += GEAR_FRAME, step++) {
        SDL_memset(frame, 0, sizeof frame);
        if (i + GEAR_FRAME <= n) SDL_memcpy(frame, speech + i, sizeof frame);
        gear_pipeline_frame(p, frame, NULL);
        message = gear_pipeline_poll(p);
        const char *draft = gear_pipeline_draft(p);
        if (*draft && SDL_strcmp(draft, last)) updates++, SDL_strlcpy(last, draft, sizeof last);
        Uint64 due = start + (Uint64)(step + 1) * 10000000;
        if (SDL_GetTicksNS() < due) SDL_DelayNS(due - SDL_GetTicksNS());
    }
    printf("drafts: %d, the last \"%s\"\n", updates, last);
    CHECK(message && updates >= 2 && !*gear_pipeline_draft(p));
    if (message) {
        lower_letters(message), lower_letters(last);
        CHECK(!SDL_strcmp(message, utterance_text));
        CHECK(!SDL_strncmp(last, "well now", 8));
    }
    SDL_free(message);
    gear_pipeline_free(p);
    // Heard as it plays through the speakers, the utterance is the speakers' sound and makes no message;
    // said while the speakers play something else (the same audio backwards), it is still the message.
    float *backwards = SDL_malloc((size_t)n * sizeof(float));
    for (int i = 0; i < n; i++) backwards[i] = speech[n - 1 - i];
    for (int other = 0; other < 2; other++) {
        gear_pipeline *q = gear_pipeline_new(voice, vad, hear, NULL, &lp, 1.0);
        char *got = NULL;
        float played[GEAR_FRAME];
        for (int i = 0; !got && i < n + GEAR_RATE * 4; i += GEAR_FRAME) {
            SDL_memset(frame, 0, sizeof frame);
            SDL_memset(played, 0, sizeof played);
            if (i + GEAR_FRAME <= n) SDL_memcpy(frame, speech + i, sizeof frame), SDL_memcpy(played, (other ? backwards : speech) + i, sizeof played);
            while (gear_hear_pending(hear)) SDL_Delay(1);
            gear_pipeline_frame(q, frame, played);
            got = gear_pipeline_poll(q);
        }
        if (got) lower_letters(got);
        if (other) CHECK(got && !SDL_strcmp(got, utterance_text));
        else CHECK(!got);
        SDL_free(got);
        gear_pipeline_free(q);
    }
    SDL_free(backwards);
    gear_vad_free(vad);
    // What was heard is kept: the piece's file, its text, and the message's outcome.
    char log[1300];
    SDL_snprintf(log, sizeof log, "%s/heard.jsonl", keep);
    char *lines = SDL_LoadFile(log, NULL);
    CHECK(lines && SDL_strstr(lines, "\"model\":\"test-model\"") && SDL_strstr(lines, "\"outcome\":\"sent\""));
    char *file = lines ? SDL_strstr(lines, "\"file\":\"") : NULL;
    if (file) {
        char kept[1400];
        file += 8;
        size_t n2 = (size_t)(SDL_strchr(file, '"') - file);
        SDL_snprintf(kept, sizeof kept, "%s/%.*s", keep, (int)n2, file);
        int samples;
        float *back = gear_wav_read(kept, GEAR_RATE, &samples);
        CHECK(back && samples > GEAR_RATE);
        SDL_free(back);
    } else CHECK(file);
    SDL_free(lines);

    // Saying and interrupting, with headphones (the microphone hears only the user), then with
    // loudspeakers (it also hears the reply) and the echo canceller, and as a control loudspeakers
    // without it, where the reply should cut itself off.
    char model[1200], voice_path[1200];
    gear_model_path(dir, GEAR_MODEL_SAY, model, sizeof model);
    gear_model_path(dir, GEAR_MODEL_VOICE_ALBA, voice_path, sizeof voice_path);
    gs_voice_free(voice);
    double heard, interrupted, quiet;
    if (duplex(model, voice_path, vad_path, hear, speech, n, HEADPHONES, &heard, &interrupted, &quiet, NULL)) {
        CHECK(heard > 0 && heard < 1.0);  // the reply starts within a second
        // The utterance's speech begins 0.56 s into the file, so 2.56 s into the test; Silero hears it
        // within 0.2 s, and once it has gone on for 0.3 s more the reading stops at once.
        CHECK(interrupted >= 2.86 && interrupted < 3.06);
        CHECK(quiet >= interrupted && quiet - interrupted < 0.05);
    }
    // A 0.15 s burst of speech, as long as the reply's own sound that once got past the canceller,
    // leaves the reply playing.
    if (duplex(model, voice_path, vad_path, hear, speech, (int)(0.71 * GEAR_RATE), HEADPHONES, &heard, &interrupted, &quiet, NULL))
        CHECK(interrupted < 0);
    if (gs_voice_has_echo()) {
        if (duplex(model, voice_path, vad_path, hear, speech, n, LOUDSPEAKERS, &heard, &interrupted, &quiet, NULL))
            CHECK(interrupted >= 2.86 && interrupted < 3.06);  // the reply's echo is not taken for talking
        if (duplex(model, voice_path, vad_path, hear, speech, n, LOUDSPEAKERS_NO_CANCELLER, &heard, &interrupted, &quiet, NULL))
            CHECK(interrupted > 0 && interrupted < 2.56);  // without the canceller the reply stops itself
    } else printf("echo tests skipped: build with -Decho=true\n");
    // Nobody talks, and the microphone hears the reply past the canceller. The reply stops itself, and
    // what was heard of it is a message unless gear kept what it played to compare the words with.
    if (duplex(model, voice_path, vad_path, hear, speech, 0, LOUDSPEAKERS_NO_CANCELLER, &heard, &interrupted, &quiet, &message)) CHECK(message);
    SDL_free(message);
    if (duplex(model, voice_path, vad_path, hear, speech, 0, LOUDSPEAKERS_LET_THROUGH, &heard, &interrupted, &quiet, &message)) CHECK(!message);
    SDL_free(message);
    gear_hear_close(hear);
    SDL_free(speech);
}

// The median pitch of what is voiced in 16 kHz mono x, by autocorrelation over 40 ms frames.
static double median_pitch(const float *x, int n) {
    enum { FRAME = 640, LO = 16000 / 400, HI = 16000 / 70 };
    static double found[2000];
    int count = 0;
    for (int i = 0; i + FRAME <= n && count < 2000; i += FRAME / 2) {
        double mean = 0, energy = 0, best = 0;
        for (int k = 0; k < FRAME; k++) mean += x[i + k] / FRAME;
        for (int k = 0; k < FRAME; k++) energy += (x[i + k] - mean) * (x[i + k] - mean);
        if (energy / FRAME < 0.02 * 0.02) continue;
        int lag = 0;
        for (int l = LO; l < HI; l++) {
            double ac = 0;
            for (int k = 0; k + l < FRAME; k++) ac += (x[i + k] - mean) * (x[i + k + l] - mean);
            if (ac > best) best = ac, lag = l;
        }
        if (lag && best > 0.5 * energy) found[count++] = 16000.0 / lag;
    }
    for (int a = 1; a < count; a++)
        for (int b = a; b > 0 && found[b - 1] > found[b]; b--) { double t = found[b]; found[b] = found[b - 1]; found[b - 1] = t; }
    return count ? found[count / 2] : 0;
}

// A sentence played through the mix in real time, as 16 kHz mono, and its median pitch.
static double say_pitch(gear_say *say, const char *text) {
    enum { STEP = 480, MAX = 16000 * 20 };
    static float mono[MAX];
    float lr[STEP * 2];
    int factor = gs_mix_rate() / 16000, n = 0;
    gear_say_speak(say, text);
    Uint64 start = SDL_GetTicksNS();
    for (int step = 0; step < 2000 && (step < 100 || gear_say_busy(say)); step++) {
        gs_mix_render(lr, STEP);
        for (int i = 0; i + factor <= STEP && n < MAX; i += factor) {
            float sum = 0;
            for (int k = 0; k < factor; k++) sum += (lr[2 * (i + k)] + lr[2 * (i + k) + 1]) / 2;
            mono[n++] = sum / (float)factor;
        }
        Uint64 due = start + (Uint64)(step + 1) * 10000000;
        if (SDL_GetTicksNS() < due) SDL_DelayNS(due - SDL_GetTicksNS());
    }
    return median_pitch(mono, n);
}

// Switching voice between replies: alba is a low voice and mary a higher one (their median pitches
// were 121 and 182 Hz on five agent replies; this estimate of one passage reads them less far
// apart), and switching back gives alba again.
static void test_voice_switch(const char *dir) {
    char model[1200], alba[1200], mary[1200], error[512];
    gear_model_path(dir, GEAR_MODEL_SAY, model, sizeof model);
    gear_model_path(dir, GEAR_MODEL_VOICE_ALBA, alba, sizeof alba);
    gear_model_path(dir, GEAR_MODEL_VOICE_MARY, mary, sizeof mary);
    gear_say *say = gear_say_open(model, alba, 2, NULL, error, sizeof error);
    CHECK(say);
    if (!say) { fprintf(stderr, "%s\n", error); return; }
    const char *text = "The tests pass, and the build is ready to publish. I read the log and found nothing else to fix, so I will push it now.";
    double low = say_pitch(say, text);
    gear_say_set_voice(say, mary);
    double high = say_pitch(say, text);
    gear_say_set_voice(say, alba);
    double again = say_pitch(say, text);
    printf("voices: alba %.0f Hz, mary %.0f Hz, alba again %.0f Hz\n", low, high, again);
    CHECK(low > 0 && high > low * 1.15 && again < high / 1.15);
    gear_say_close(say);

    // The choice is remembered, and anything but mary reads as alba.
    char tmp[1100], *pref = SDL_GetPrefPath("wakamex", "gear-tests");
    SDL_snprintf(tmp, sizeof tmp, "%svoice-%llu", pref, (unsigned long long)SDL_GetTicksNS());
    SDL_free(pref);
    SDL_CreateDirectory(tmp);
    CHECK(gear_voice_remembered(tmp) == GEAR_MODEL_VOICE_ALBA);
    gear_voice_remember(tmp, GEAR_MODEL_VOICE_MARY);
    CHECK(gear_voice_remembered(tmp) == GEAR_MODEL_VOICE_MARY);
    gear_voice_remember(tmp, GEAR_MODEL_VOICE_ALBA);
    CHECK(gear_voice_remembered(tmp) == GEAR_MODEL_VOICE_ALBA);
}

// The messages sent outlast a restart, the newest kept, and a line cut short is passed over.
static void test_transcript(const char *tmp) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%smessages-%llu.jsonl", tmp, (unsigned long long)SDL_GetTicksNS());
    gear_transcript *t = gear_transcript_open(path, 2);
    CHECK(gear_transcript_count(t) == 0);
    gear_transcript_add(t, "first", "claude");
    gear_transcript_add(t, "second, with \"quotes\"", "");
    gear_transcript_close(t);
    SDL_IOStream *io = SDL_IOFromFile(path, "ab");
    SDL_WriteIO(io, "{\"at\":\"2026", 11);
    SDL_CloseIO(io);
    t = gear_transcript_open(path, 2);
    CHECK(gear_transcript_count(t) == 2);
    CHECK(!SDL_strcmp(gear_transcript_at(t, 0)->text, "first") && !SDL_strcmp(gear_transcript_at(t, 0)->to, "claude"));
    CHECK(SDL_strlen(gear_transcript_at(t, 0)->at) == 16);
    gear_transcript_add(t, "third", "codex");
    CHECK(gear_transcript_count(t) == 2 && !SDL_strcmp(gear_transcript_at(t, 0)->text, "second, with \"quotes\""));
    gear_transcript_close(t);
    t = gear_transcript_open(path, 10);
    CHECK(gear_transcript_count(t) == 3 && !SDL_strcmp(gear_transcript_at(t, 2)->text, "third"));
    gear_transcript_close(t);
    SDL_RemovePath(path);
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
    gear_relay *r = gear_relay_new(a.name);
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

static void speaks(const char *reply, size_t limit, const char *expected) {
    char *got = gear_speakable(reply, limit);
    if (SDL_strcmp(got, expected)) {
        fprintf(stderr, "speakable:\n  reply:    %s\n  expected: %s\n  got:      %s\n", reply, expected, got);
        failures++;
    }
    SDL_free(got);
}

static void test_speakable(void) {
    speaks("I found the bug in `src/listen.c`. See https://github.com/wakamex/gesso/pull/12 for details.", 0,
           "I found the bug in listen.c. See a github dot com link for details.");
    speaks("Here is the fix:\n\n```c\nint a;\nint b;\n```\n\nWant me to commit it?", 0,
           "Here is the fix: A code block of 2 lines. Want me to commit it?");
    speaks("```\nls -la\n```", 0, "A one-line code block.");
    speaks("Results:\n\n| Model | WER |\n|---|---|\n| a | 1% |\n| b | 2% |\n\nDone.", 0, "Results: A table of 2 rows. Done.");
    speaks("Next steps:\n- Fix the **countdown**\n- Run `zig build test`\n1. Commit it", 0,
           "Next steps: Fix the countdown. Run zig build test. Commit it.");
    speaks("## Summary\nAll tests pass", 0, "Summary. All tests pass.");
    speaks("Call `gear_listen_new` and then gs_voice_frame.", 0, "Call gear listen new and then gs voice frame.");
    speaks("Keep it and/or drop half of 1/2 in tmp/live.", 0, "Keep it and or drop half of 1/2 in tmp live.");
    speaks("[The README](README.md) explains it, and *so* does _the brief_.", 0, "The README explains it, and so does the brief.");
    speaks("It was in ~/.config/gear/settings.json and C:\\Users\\me\\gear.exe", 0, "It was in settings.json and gear.exe.");
    speaks("First line\nsecond line of the same paragraph.\n\nNew paragraph", 0,
           "First line second line of the same paragraph. New paragraph.");
    speaks("> quoted advice\n\n---\n\n- [x] done item", 0, "quoted advice. done item.");
    speaks("One. Two. Three. Four.", 10, "One. Two. The rest is not read aloud.");
    speaks("It prints `Hello, world!`.", 0, "It prints Hello, world!");
    speaks("", 0, "");
    speaks("Block 139 failed at 575, and 7,445 s is 83.5% of the 0.34 budget.", 0,
           "Block one hundred thirty nine failed at five hundred seventy five, and seven thousand four hundred forty five "
           "seconds is eighty three point five percent of the zero point three four budget.");
    speaks("Pin 6ad685e5 for v0.5.0, port-0073, 41 GiB and 1.55x on w3sim.", 0,
           "Pin six a d six eight five for v zero point five point zero, port zero zero seven three, forty one gigabytes and one "
           "point five five times on w three sim.");
    speaks("It ran 1905 to 2021, on 2026-10-28 at 12:15 and 02:05, for $8 and -0.44 (~3 ms).", 0,
           "It ran nineteen oh five to twenty twenty one, on October twenty eighth, twenty twenty six at twelve fifteen and "
           "two oh five, for eight dollars and minus zero point four four (about three milliseconds).");
    speaks("The record is at dmarc.example.org, not notes.md.", 0, "The record is at dmarc dot example dot org, not notes.md.");
    speaks("Costs $0.70, $1.00 or $1,249.50 for claude-opus-5-5 under m8-fogunits-wait3.", 0,
           "Costs seventy cents, one dollar or one thousand two hundred forty nine dollars and fifty cents for claude opus "
           "five five under m eight fogunits wait three.");
    speaks("Set v=DMARC1 so len + i == n & done.", 0, "Set v equals DMARC one so len plus i equals n and done.");
    // "read" in the past is written as it sounds, where grammar says it is the past.
    speaks("She read only the first page. Everything I say is read aloud, and it has never been read twice.", 0,
           "She red only the first page. Everything I say is red aloud, and it has never been red twice.");
    speaks("It just read the log, I've read it too, and what it read was short.", 0, "It just red the log, I've red it too, and what it red was short.");
    speaks("I read the file. Read it first, then let it read the rest; it can read all of it, and did she read it?", 0,
           "I read the file. Read it first, then let it read the rest; it can read all of it, and did she read it?");
    speaks("The mount is read only, so give it read access. I will read it, and they read it daily.", 0,
           "The mount is read only, so give it read access. I will read it, and they read it daily.");
    speaks("See Chess.com, e.g. the 21st of 4-9 at 192.168.2.1.", 0,
           "See Chess dot com, e.g. the twenty first of four to nine at one hundred ninety two point one hundred sixty eight point two point one.");
}

static void test_speech(void) {
    CHECK(gear_speech_checked() < 1e-4f);  // the model's table gives the scores the fitting tool got
    // Ten frames of a piece: Silero has 8 at 0.5 or more, 4 of those at 0.95 or more and 2 at 0.99; the
    // second detector has 5 at 0.5 or more.
    static const float silero[] = { 0.6f, 0.7f, 0.96f, 0.97f, 0.99f, 1.0f, 0.9f, 0.85f, 0.2f, 0.1f };
    static const float second[] = { 0.1f, 0.6f, 0.95f, 0.99f, 1.0f, 0.9f, 0.4f, 0.3f, 0.0f, 0.0f };
    gear_speech_heard h = { 0 };
    for (int i = 0; i < 10; i++) gear_speech_frame(&h, silero[i], second[i]);
    gear_hear_scores scores = { 2, -1.5f, 3.0f, 2.0f, -1.0f, 0.8f, 0.9f };
    float in[GEAR_SPEECH_INPUTS];
    gear_speech_inputs(&h, &scores, in);
    CHECK(SDL_fabsf(in[0] - 4 / 8.f) < 1e-6f && SDL_fabsf(in[1] - 2 / 8.f) < 1e-6f && SDL_fabsf(in[4] - 1.0f) < 1e-6f);
    CHECK(in[6] == -1.5f && in[7] == 3.0f && in[11] == 0.9f);
    CHECK(SDL_fabsf(in[12] - 5 / 8.f) < 1e-6f && SDL_fabsf(in[15] - 1.0f) < 1e-6f);
    float score = gear_speech_score(in);
    CHECK(score >= 0 && score <= 1);
}

int main(int argc, char **argv) {
    const char *data = argc > 1 ? argv[1] : "tests";
    SDL_Init(SDL_INIT_AUDIO);
    char *tmp = SDL_GetPrefPath("wakamex", "gear-tests");
    test_listen();
    test_speakable();
    test_speech();
    test_wav(tmp);
    test_transcript(tmp);
    test_agent_refused();
    const char *agent = SDL_getenv("GEAR_AGENT");
    if (agent && *agent) test_agent_round_trip(agent);
    else printf("agent round trip skipped: GEAR_AGENT is not set\n");
    const char *dir = SDL_getenv("GEAR_MODELS");
    if (dir && gear_models_present(dir)) test_end_to_end(dir, data), test_voice_switch(dir);
    else printf("end-to-end tests skipped: GEAR_MODELS does not hold the models\n");
    SDL_free(tmp);
    SDL_Quit();
    if (failures) fprintf(stderr, "%d failed\n", failures);
    else printf("all passed\n");
    return failures ? 1 : 0;
}
