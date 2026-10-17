// gear's command line. The live mode runs the spoken loop on the microphone and speakers; the file
// modes run the same pipeline on recordings, so it can be tested without either.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "gs_mix.h"
#include "gs_voice.h"
#include "agent.h"
#include "hear.h"
#include "listen.h"
#include "models.h"
#include "pipeline.h"
#include "relay.h"
#include "say.h"
#include "speakable.h"
#include "wav.h"
#include "window.h"

enum { MIX_RATE = 48000, MIX_STEP = MIX_RATE / 100 };
// The most of a reply read aloud, in characters of speakable text: about a minute and a quarter.
enum { READ_LIMIT = 1200 };  // the mix, rendered 10 ms at a time offline

static const char usage[] =
    "usage: gear fetch                         download the models\n"
    "       gear window [--to AGENT]           the window: talk, and hear the replies\n"
    "       gear window --demo --shot F.png    a picture of the window with a made-up exchange\n"
    "       gear agents                        the agents Wakterm can reach\n"
    "       gear listen --to AGENT             talk to AGENT; its replies are read aloud\n"
    "       gear listen [--speak-back]         talk; each message is printed, and read back with --speak-back\n"
    "       gear send --to AGENT TEXT          send TEXT to AGENT and print its reply\n"
    "       gear read TEXT|-                   what would be read aloud of a reply (- reads standard input)\n"
    "       gear hear FILE.wav                 the pipeline on a recording, as if spoken\n"
    "       gear say TEXT OUT.wav              speak TEXT through the player, recorded to OUT.wav\n"
    "       gear interrupt TEXT MIC.wav OUT.wav  speak TEXT while MIC.wav plays as the microphone\n"
    "options: --countdown SECONDS (3)  --hear-threads N (1)  --say-threads N (2)  --no-echo\n"
    "models:  GEAR_MODELS, or a models folder in the user's data folder for gear\n";

static struct { double countdown; int hear_threads, say_threads; bool speak_back, no_echo; const char *to, *shot; bool demo; } opt = { 3.0, 1, 2, false, false, NULL, NULL, false };

static void progress(void *user, long long received, long long total, double eta) {
    if (eta >= 0) fprintf(stderr, "\r%5.1f%% of %.0f MB, %.0f s left   ", 100.0 * (double)received / (double)total, (double)total / 1e6, eta);
    else fprintf(stderr, "\r%5.1f%% of %.0f MB   ", 100.0 * (double)received / (double)total, (double)total / 1e6);
}

static const char *models(void) {
    const char *dir = gear_models_dir();
    if (!dir) { fprintf(stderr, "gear: no folder for the models: %s\n", SDL_GetError()); return NULL; }
    if (!gear_models_present(dir)) { fprintf(stderr, "gear: the models are not in %s; run gear fetch\n", dir); return NULL; }
    return dir;
}

static int fetch(void) {
    const char *dir = gear_models_dir();
    if (!dir) { fprintf(stderr, "gear: no folder for the models: %s\n", SDL_GetError()); return 1; }
    for (int m = 0; m < GEAR_MODEL_COUNT; m++) fprintf(stderr, "%s: %s\n", gear_model_info(m)->name, gear_model_info(m)->about);
    SDL_AtomicInt stop = { 0 };
    char error[512];
    bool ok = gear_models_fetch(dir, &stop, progress, NULL, error, sizeof error);
    fprintf(stderr, "\n");
    if (!ok) { fprintf(stderr, "gear: %s\n", error); return 1; }
    printf("models in %s\n", dir);
    return 0;
}

static gear_hear *open_hear(const char *dir) {
    char path[1200], error[512];
    gear_model_path(dir, GEAR_MODEL_HEAR, path, sizeof path);
    gear_hear *h = gear_hear_open(path, opt.hear_threads, error, sizeof error);
    if (!h) fprintf(stderr, "gear: %s\n", error);
    return h;
}

static gear_say *open_say(const char *dir, gs_voice *echo) {
    char model[1200], voice[1200], error[512];
    gear_model_path(dir, GEAR_MODEL_SAY, model, sizeof model);
    gear_model_path(dir, GEAR_MODEL_VOICE, voice, sizeof voice);
    gear_say *s = gear_say_open(model, voice, opt.say_threads, echo, error, sizeof error);
    if (!s) fprintf(stderr, "gear: %s\n", error);
    return s;
}

static gs_voice *open_voice(void) {
    bool echo = !opt.no_echo && gs_voice_has_echo();
    return gs_voice_new(GEAR_RATE, GS_VOICE_SPEECH | (echo ? GS_VOICE_ECHO : 0));
}

static int agents(void) {
    enum { MAX = 512 };
    static gear_agent all[MAX];
    char error[512];
    int n = gear_agents_list(all, MAX, error, sizeof error);
    if (n < 0) { fprintf(stderr, "gear: %s\n", error); return 1; }
    for (int i = 0; i < n; i++) printf("%-40s %-8s %s\n", all[i].name, all[i].harness, all[i].status);
    return 0;
}

static int send_text(const char *text) {
    gear_agent a;
    char error[512];
    if (!opt.to) { fputs("gear: send needs --to AGENT\n", stderr); return 2; }
    if (!gear_agent_find(opt.to, &a, error, sizeof error)) { fprintf(stderr, "gear: %s\n", error); return 1; }
    gear_relay *r = gear_relay_new(&a);
    gear_relay_send(r, text);
    char *reply = NULL;
    bool failed = false;
    while (!(reply = gear_relay_take(r, &failed, error, sizeof error)) && !failed) SDL_Delay(100);
    gear_relay_free(r);
    if (failed) { fprintf(stderr, "gear: %s\n", error); return 1; }
    printf("%s\n", reply);
    SDL_free(reply);
    return 0;
}

static int read_text(const char *text) {
    char *input = NULL;
    if (!SDL_strcmp(text, "-")) {
        size_t len = 0, cap = 4096;
        input = SDL_malloc(cap);
        for (size_t n; (n = fread(input + len, 1, cap - len - 1, stdin)) > 0;) {
            len += n;
            if (len + 1 == cap) input = SDL_realloc(input, cap *= 2);
        }
        input[len] = 0;
    }
    char *spoken = gear_speakable(input ? input : text, READ_LIMIT);
    printf("%s\n", spoken);
    SDL_free(spoken);
    SDL_free(input);
    return 0;
}

// The live loop: the microphone into the pipeline, messages printed and, with --speak-back, read back.
static int listen_live(void) {
    const char *dir = models();
    if (!dir) return 1;
    gs_voice *voice = open_voice();
    gear_hear *hear = open_hear(dir);
    gear_say *say = NULL;
    if (!hear) return 1;
    gear_relay *relay = NULL;
    if (opt.to) {
        gear_agent a;
        char error[512];
        if (!gear_agent_find(opt.to, &a, error, sizeof error)) { fprintf(stderr, "gear: %s\n", error); return 1; }
        relay = gear_relay_new(&a);
        fprintf(stderr, "gear: talking to %s (%s)\n", a.name, a.harness);
    }
    if (opt.speak_back || relay) {
        if (!gs_mix_open(MIX_RATE)) { fprintf(stderr, "gear: no playback device: %s\n", SDL_GetError()); return 1; }
        if (!(say = open_say(dir, voice))) return 1;
    }
    const SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, GEAR_RATE };
    SDL_AudioStream *mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL);
    if (!mic) { fprintf(stderr, "gear: no microphone: %s\n", SDL_GetError()); return 1; }
    SDL_ResumeAudioStreamDevice(mic);
    fprintf(stderr, "gear: listening%s; Ctrl+C quits\n", gs_voice_has_echo() && !opt.no_echo ? " with echo cancellation" : "");
    gear_listen_params lp = gear_listen_defaults();
    gear_pipeline *p = gear_pipeline_new(voice, hear, say, &lp, opt.countdown);
    float frame[GEAR_FRAME];
    char shown[4096] = "";
    for (bool quit = false; !quit;) {
        for (SDL_Event e; SDL_PollEvent(&e);) quit |= e.type == SDL_EVENT_QUIT;
        while (SDL_GetAudioStreamAvailable(mic) >= (int)sizeof frame) {
            SDL_GetAudioStreamData(mic, frame, sizeof frame);
            gear_pipeline_frame(p, frame);
        }
        char *message = gear_pipeline_poll(p);
        if (SDL_strcmp(shown, gear_pipeline_text(p))) {
            SDL_strlcpy(shown, gear_pipeline_text(p), sizeof shown);
            if (*shown) fprintf(stderr, "… %s\n", shown);
        }
        if (message) {
            printf("> %s\n", message);
            fflush(stdout);
            if (relay) gear_relay_send(relay, message);
            else if (say) gear_say_speak(say, message);
            SDL_free(message);
        }
        char error[512];
        bool failed;
        for (char *reply; relay && ((reply = gear_relay_take(relay, &failed, error, sizeof error)) || failed);) {
            if (failed) { fprintf(stderr, "gear: %s\n", error); break; }
            printf("%s\n", reply);
            fflush(stdout);
            char *spoken = gear_speakable(reply, READ_LIMIT);
            gear_say_speak(say, spoken);
            SDL_free(spoken);
            SDL_free(reply);
        }
        SDL_Delay(10);
    }
    gear_pipeline_free(p);
    gear_relay_free(relay);
    SDL_DestroyAudioStream(mic);
    gear_say_close(say);
    gear_hear_close(hear);
    gs_voice_free(voice);
    gs_mix_close();
    return 0;
}

// A recording through the pipeline at full speed, then silence until its message is sent.
static int hear_file(const char *path) {
    const char *dir = models();
    if (!dir) return 1;
    int n;
    float *audio = gear_wav_read(path, GEAR_RATE, &n);
    if (!audio) { fprintf(stderr, "gear: cannot read %s: %s\n", path, SDL_GetError()); return 1; }
    gear_hear *hear = open_hear(dir);
    if (!hear) return 1;
    gs_voice *voice = open_voice();
    gear_listen_params lp = gear_listen_defaults();
    gear_pipeline *p = gear_pipeline_new(voice, hear, NULL, &lp, opt.countdown);
    float frame[GEAR_FRAME];
    int sent = 0;
    for (int i = 0;; i += GEAR_FRAME) {
        int k = i < n ? SDL_min(GEAR_FRAME, n - i) : 0;
        SDL_memset(frame, 0, sizeof frame);
        SDL_memcpy(frame, audio + (i < n ? i : 0), sizeof(float) * (size_t)k);
        // Frames come faster than speech here. Live, a piece is transcribed long before the next
        // second of audio arrives, so the file waits for each transcription to behave the same.
        while (gear_hear_pending(hear)) SDL_Delay(1);
        gear_pipeline_frame(p, frame);
        char *message = gear_pipeline_poll(p);
        if (message) {
            printf("%.2f s: %s\n", gear_pipeline_time(p), message);
            SDL_free(message);
            sent++;
        }
        if (i >= n && !gear_pipeline_hearing(p) && !gear_hear_pending(hear) && !*gear_pipeline_text(p) && gear_pipeline_time(p) > (double)n / GEAR_RATE + 1) break;
    }
    gear_pipeline_free(p);
    gs_voice_free(voice);
    gear_hear_close(hear);
    SDL_free(audio);
    return sent ? 0 : 1;
}

typedef struct { float *lr; int frames, cap; } recording;

static void record_step(recording *r) {
    if (r->frames + MIX_STEP > r->cap) {
        r->cap = r->cap ? r->cap * 2 : MIX_RATE * 10;
        r->lr = SDL_realloc(r->lr, (size_t)r->cap * 2 * sizeof(float));
    }
    gs_mix_render(r->lr + (size_t)r->frames * 2, MIX_STEP);
    r->frames += MIX_STEP;
}

static double step_level(const recording *r) {
    double sum = 0;
    for (int i = 0; i < MIX_STEP * 2; i++) sum += (double)r->lr[(size_t)(r->frames - MIX_STEP) * 2 + i] * r->lr[(size_t)(r->frames - MIX_STEP) * 2 + i];
    return SDL_sqrt(sum / (MIX_STEP * 2));
}

// TEXT through the player, its mix rendered in real time (10 ms a step) as a device would take it.
static int say_file(const char *text, const char *out) {
    const char *dir = models();
    if (!dir) return 1;
    gear_say *say = open_say(dir, NULL);
    if (!say) return 1;
    recording r = { 0 };
    Uint64 start = SDL_GetTicksNS();
    gear_say_speak(say, text);
    double heard = -1;
    for (int step = 0; gear_say_busy(say) || step < 10; step++) {
        record_step(&r);
        if (heard < 0 && step_level(&r) > 1e-4) heard = (double)(SDL_GetTicksNS() - start) / 1e9;
        Uint64 due = start + (Uint64)(step + 1) * 10000000;
        if (SDL_GetTicksNS() < due) SDL_DelayNS(due - SDL_GetTicksNS());
    }
    double first, audio;
    gear_say_last(say, &first, &audio);
    printf("first audio from Pocket %.3f s, first sound in the mix %.3f s, %.2f s of speech in %.2f s\n", first, heard, audio, (double)r.frames / MIX_RATE);
    bool ok = gear_wav_write(out, r.lr, r.frames, 2, MIX_RATE);
    gear_say_close(say);
    SDL_free(r.lr);
    return ok ? 0 : 1;
}

// TEXT is read while MIC.wav plays as the microphone, both in real time; talking in MIC.wav should
// stop the reading. Reports when the pipeline heard the talking start and when the player went quiet.
static int interrupt_file(const char *text, const char *mic_path, const char *out) {
    const char *dir = models();
    if (!dir) return 1;
    int n;
    float *mic = gear_wav_read(mic_path, GEAR_RATE, &n);
    if (!mic) { fprintf(stderr, "gear: cannot read %s: %s\n", mic_path, SDL_GetError()); return 1; }
    gs_voice *voice = open_voice();
    gear_hear *hear = open_hear(dir);
    gear_say *say = hear ? open_say(dir, voice) : NULL;
    if (!say) return 1;
    gear_listen_params lp = gear_listen_defaults();
    gear_pipeline *p = gear_pipeline_new(voice, hear, say, &lp, opt.countdown);
    recording r = { 0 };
    float frame[GEAR_FRAME];
    double stopped_at = -1, quiet_at = -1;
    Uint64 start = SDL_GetTicksNS();
    gear_say_speak(say, text);
    for (int step = 0; step * GEAR_FRAME < n; step++) {
        record_step(&r);
        SDL_memcpy(frame, mic + step * GEAR_FRAME, sizeof(float) * (size_t)SDL_min(GEAR_FRAME, n - step * GEAR_FRAME));
        int before = gear_pipeline_interruptions(p);
        gear_pipeline_frame(p, frame);
        double t = (double)(step + 1) / 100.0;
        if (gear_pipeline_interruptions(p) > before) stopped_at = t;
        if (stopped_at >= 0 && quiet_at < 0 && step_level(&r) < 1e-4) quiet_at = t;
        SDL_free(gear_pipeline_poll(p));
        Uint64 due = start + (Uint64)(step + 1) * 10000000;
        if (SDL_GetTicksNS() < due) SDL_DelayNS(due - SDL_GetTicksNS());
    }
    if (stopped_at < 0) printf("not interrupted\n");
    else printf("talking heard at %.2f s, the reading stopped there and the player was quiet at %.2f s\n", stopped_at, quiet_at);
    gear_wav_write(out, r.lr, r.frames, 2, MIX_RATE);
    gear_pipeline_free(p);
    gear_say_close(say);
    gear_hear_close(hear);
    gs_voice_free(voice);
    SDL_free(mic);
    SDL_free(r.lr);
    return stopped_at >= 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *args[4] = { 0 };
    int nargs = 0;
    for (int i = 1; i < argc; i++) {
        if (!SDL_strcmp(argv[i], "--countdown") && i + 1 < argc) opt.countdown = SDL_atof(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--hear-threads") && i + 1 < argc) opt.hear_threads = SDL_atoi(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--say-threads") && i + 1 < argc) opt.say_threads = SDL_atoi(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--speak-back")) opt.speak_back = true;
        else if (!SDL_strcmp(argv[i], "--no-echo")) opt.no_echo = true;
        else if (!SDL_strcmp(argv[i], "--to") && i + 1 < argc) opt.to = argv[++i];
        else if (!SDL_strcmp(argv[i], "--shot") && i + 1 < argc) opt.shot = argv[++i];
        else if (!SDL_strcmp(argv[i], "--demo")) opt.demo = true;
        else if (nargs < 4 && (argv[i][0] != '-' || !argv[i][1])) args[nargs++] = argv[i];
        else { fputs(usage, stderr); return 2; }
    }
    if (!nargs || !SDL_strcmp(args[0], "window")) {
        if (nargs > 1) { fputs(usage, stderr); return 2; }
        const char *dir = opt.demo ? "" : gear_models_dir();
        gear_window_options o = { dir ? dir : "", opt.to, opt.countdown, opt.hear_threads, opt.say_threads, opt.no_echo, opt.shot, opt.demo };
        return gear_window(&o);
    }
    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS)) { fprintf(stderr, "gear: %s\n", SDL_GetError()); return 1; }
    int rc = 2;
    if (!SDL_strcmp(args[0], "fetch") && nargs == 1) rc = fetch();
    else if (!SDL_strcmp(args[0], "agents") && nargs == 1) rc = agents();
    else if (!SDL_strcmp(args[0], "send") && nargs == 2) rc = send_text(args[1]);
    else if (!SDL_strcmp(args[0], "read") && nargs == 2) rc = read_text(args[1]);
    else if (!SDL_strcmp(args[0], "listen") && nargs == 1) rc = listen_live();
    else if (!SDL_strcmp(args[0], "hear") && nargs == 2) rc = hear_file(args[1]);
    else if (!SDL_strcmp(args[0], "say") && nargs == 3) rc = say_file(args[1], args[2]);
    else if (!SDL_strcmp(args[0], "interrupt") && nargs == 4) rc = interrupt_file(args[1], args[2], args[3]);
    else fputs(usage, stderr);
    SDL_Quit();
    return rc;
}
