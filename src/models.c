#include "models.h"
#include "gs_eta.h"
#include "gs_http.h"
#include "gs_sha256.h"

static const gear_model_file files[GEAR_MODEL_COUNT] = {
    [GEAR_MODEL_HEAR] = {
        "parakeet-tdt_ctc-110m-Q4_K_M.gguf",
        "https://huggingface.co/handy-computer/parakeet-tdt_ctc-110m-gguf/resolve/1c3f0d5378fdd358905cc410a93ed9e64f52d170/parakeet-tdt_ctc-110m-Q4_K_M.gguf",
        89989600, "486414fd90185a8c8a4ced7c123cfb133ff4f7958426c6b8bd9049946b56b448",
        "NVIDIA Parakeet TDT-CTC 110M, English speech recognition (CC-BY-4.0)",
    },
    [GEAR_MODEL_SAY] = {
        "pocket-tts-english_2026-09-Q8_0.gguf",
        "https://huggingface.co/Serveurperso/pocket-tts-GGUF/resolve/b5a0115aee9658c110d747debb03edd548c4b9c7/pocket-tts-english_2026-09-Q8_0.gguf",
        138498080, "1c91f8cd54efd089236737a338e998b9cd1bc9c3b1d6bc5bd038a99f2527deba",
        "Kyutai Pocket TTS, English speech synthesis (CC-BY-4.0)",
    },
    [GEAR_MODEL_VOICE] = {
        "alba.safetensors",
        "https://huggingface.co/kyutai/pocket-tts-without-voice-cloning/resolve/575d14372df89a4534bcb989870a34e77544b09f/languages/english_2026-09/embeddings/alba.safetensors",
        6195000, "d291428b416d6c36a1de7835e51dbe1e334b75e5af512bb18dd23a1047fe8f3b",
        "Kyutai's alba voice for Pocket TTS (CC-BY-4.0)",
    },
};

const gear_model_file *gear_model_info(gear_model m) { return &files[m]; }

const char *gear_models_dir(void) {
    static char dir[1024];
    if (dir[0]) return dir;
    const char *env = SDL_getenv("GEAR_MODELS");
    if (env && *env) SDL_strlcpy(dir, env, sizeof dir);
    else {
        char *pref = SDL_GetPrefPath("wakamex", "gear");
        if (!pref) return NULL;
        SDL_snprintf(dir, sizeof dir, "%smodels", pref);
        SDL_free(pref);
    }
    return SDL_CreateDirectory(dir) ? dir : NULL;
}

void gear_model_path(const char *dir, gear_model m, char *out, size_t size) { SDL_snprintf(out, size, "%s/%s", dir, files[m].name); }

static long long file_size(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE ? (long long)info.size : -1;
}

bool gear_models_present(const char *dir) {
    char path[1200];
    for (int m = 0; m < GEAR_MODEL_COUNT; m++) {
        gear_model_path(dir, m, path, sizeof path);
        if (file_size(path) != files[m].bytes) return false;
    }
    return true;
}

typedef struct {
    SDL_IOStream *io;
    gs_sha256 hash;
    long long have, before, total;  // of this file; of the files before it; of all
    gs_eta *eta;
    double start;
    gear_fetch_progress *progress;
    void *user;
    bool write_failed;
} sink;

static double now(void) { return (double)SDL_GetTicksNS() / 1e9; }

static void report(sink *s) {
    if (!s->progress) return;
    long long done = s->before + s->have;
    gs_eta_done(s->eta, (double)done, now());
    double shown = gs_eta_shown(s->eta, 1.0);
    s->progress(s->user, done, s->total, shown > 0 ? SDL_max(shown - now(), 0) : -1);
}

static bool take(void *user, const Uint8 *data, size_t len) {
    sink *s = user;
    if (SDL_WriteIO(s->io, data, len) != len) { s->write_failed = true; return false; }
    gs_sha256_update(&s->hash, data, len);
    s->have += (long long)len;
    report(s);
    return true;
}

static bool fetch_one(const char *dir, const gear_model_file *f, sink *s, SDL_AtomicInt *stop, char *error, size_t error_size) {
    char dest[1200], part[1210];
    SDL_snprintf(dest, sizeof dest, "%s/%s", dir, f->name);
    SDL_snprintf(part, sizeof part, "%s.part", dest);
    for (int attempt = 0; attempt < 2; attempt++) {
        // Bytes from an earlier try are kept and hashed again, so the sum covers the whole file.
        long long have = file_size(part);
        if (have < 0 || have > f->bytes) { SDL_RemovePath(part); have = 0; }
        gs_sha256_init(&s->hash);
        s->have = 0;
        if (have > 0) {
            SDL_IOStream *old = SDL_IOFromFile(part, "rb");
            static Uint8 buf[1 << 16];
            for (size_t n; old && (n = SDL_ReadIO(old, buf, sizeof buf)) > 0;) gs_sha256_update(&s->hash, buf, n);
            if (old) SDL_CloseIO(old);
            s->have = have;
        }
        int status = 200;
        if (have < f->bytes) {
            s->io = SDL_IOFromFile(part, have > 0 ? "ab" : "wb");
            if (!s->io) { SDL_snprintf(error, error_size, "Could not write %s: %s", part, SDL_GetError()); return false; }
            gs_http_request r = { .url = f->url, .follow = true, .agent = "gear" };
            status = gs_http_stream(&r, have, take, s, stop);
            SDL_CloseIO(s->io);
        }
        if (SDL_GetAtomicInt(stop)) { SDL_strlcpy(error, "stopped", error_size); return false; }
        if (s->write_failed) { SDL_snprintf(error, error_size, "Could not write %s: the disk may be full", part); return false; }
        if (status != 200 && status != 206) {
            // A server that will not resume from here: the part is dropped and the file fetched whole.
            if (have > 0 && attempt == 0) { SDL_RemovePath(part); continue; }
            if (!status) SDL_snprintf(error, error_size, "Downloading %s failed: no answer from the server", f->name);
            else SDL_snprintf(error, error_size, "Downloading %s failed: HTTP %d", f->name, status);
            return false;
        }
        Uint8 digest[32];
        char hex[65];
        gs_sha256_final(&s->hash, digest);
        gs_sha256_hex(digest, hex);
        if (s->have == f->bytes && !SDL_strcmp(hex, f->sha256)) return SDL_RenamePath(part, dest);
        // Short of its length the connection broke, and the part stays for the next try. At its
        // length, or past it (a server that ignored the range), the bytes are wrong.
        if (s->have < f->bytes) { SDL_snprintf(error, error_size, "Downloading %s failed: the connection broke", f->name); return false; }
        SDL_RemovePath(part);
        if (have > 0 && attempt == 0) continue;
        SDL_snprintf(error, error_size, "%s did not match its published checksum", f->name);
        return false;
    }
    return false;
}

bool gear_models_fetch(const char *dir, SDL_AtomicInt *stop, gear_fetch_progress *progress, void *user, char *error, size_t error_size) {
    sink s = { .progress = progress, .user = user, .start = now() };
    char path[1200];
    // A file in place at its size arrived whole and checked; the rest are fetched, and the progress
    // and time left count only them.
    bool missing[GEAR_MODEL_COUNT];
    for (int m = 0; m < GEAR_MODEL_COUNT; m++) {
        gear_model_path(dir, m, path, sizeof path);
        missing[m] = file_size(path) != files[m].bytes;
        if (missing[m]) s.total += files[m].bytes;
    }
    s.eta = gs_eta_new((double)s.total, s.start, (gs_eta_pace){ 0 }, 20);
    bool ok = true;
    for (int m = 0; m < GEAR_MODEL_COUNT && ok; m++) {
        if (!missing[m]) continue;
        gear_model_path(dir, m, path, sizeof path);
        SDL_RemovePath(path);
        ok = fetch_one(dir, &files[m], &s, stop, error, error_size);
        s.before += files[m].bytes;
        s.have = 0;
        if (ok) report(&s);
    }
    gs_eta_free(s.eta);
    return ok;
}
