#include "models.h"
#include <string.h>
#include "gs_eta.h"
#include "gs_http.h"
#include "gs_sha256.h"

static const gear_model_file files[GEAR_MODEL_COUNT] = {
    [GEAR_MODEL_HEAR] = {
        "parakeet-ft-v1-Q4_K_M.gguf",
        "https://huggingface.co/freetranscribe/parakeet-ft-v1-gguf/resolve/453fd0047e1cfc03d9f7f2005dff0310d2a93a5a/parakeet-ft-v1-Q4_K_M.gguf",
        485425664, "a67258bc494108e1e0b4cc1989490c85f95afa43e233e1e241656f41fa80bfdc",
        "parakeet-ft-v1, a fine-tune of NVIDIA Parakeet TDT 0.6B v3, speech recognition (CC-BY-4.0)",
    },
    [GEAR_MODEL_SAY] = {
        "phonon-31m-en/model.q8.gguf",
        "https://huggingface.co/gradium/phonon-31m-en/resolve/92741c5b07d085ff840e26fea8ce43110b87852f/model.q8.gguf",
        64512352, "a7d9aad2cd6dc66473f24a08bc6d2939030ef4d751ef82349361fbef3272dbae",
        "Gradium phonon-31m-en, English speech synthesis built on Kyutai's Pocket TTS (Apache-2.0)",
    },
    [GEAR_MODEL_SAY_CONFIG] = {
        "phonon-31m-en/config.json",
        "https://huggingface.co/gradium/phonon-31m-en/resolve/92741c5b07d085ff840e26fea8ce43110b87852f/config.json",
        4634, "b5d0f88353638370d94035b7d73d7d6577c0554d78f8d7ecfd40490aae39ae85",
        "phonon-31m-en's settings (Apache-2.0)",
    },
    [GEAR_MODEL_SAY_TOKENIZER] = {
        "phonon-31m-en/tokenizer.model",
        "https://huggingface.co/gradium/phonon-31m-en/resolve/92741c5b07d085ff840e26fea8ce43110b87852f/tokenizer.model",
        65045, "82e4572006d64d483317074c22c368e87779640b6f3a9dcd07b7caae32b030ce",
        "phonon-31m-en's tokenizer (Apache-2.0)",
    },
    [GEAR_MODEL_VOICE] = {
        "delia.safetensors",
        "https://huggingface.co/gradium/phonon-voices/resolve/0ee17205f254b5abc43b266c8a0f215565c352f7/voices/nGzMHMPAWGaJFHOo.safetensors",
        256136, "a0c3dcb91b2247eca56536be9ae8e8becc1c807b1136c5fe101552d6a155b24a",
        "Gradium's Delia voice for Phonon, American English (Apache-2.0)",
    },
    [GEAR_MODEL_VAD] = {
        "silero-vad-v6.2-F32.gguf",
        "https://huggingface.co/handy-computer/silero-vad-v6.2-gguf/resolve/a9b2d61c20ae4ddf3644edb34969e8185a485448/silero-vad-v6.2-F32.gguf",
        1241056, "0d871a800c420fdd5d1b17a3aea4296d1c39608f0ff4a9295eb76457eb1c88e6",
        "Silero VAD v6.2, telling speech from other sound (MIT)",
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
    char *folder = SDL_strrchr(dest, '/');
    if (folder > dest + SDL_strlen(dir)) {  // a file kept in its own folder: the folder is made first
        *folder = 0;
        SDL_CreateDirectory(dest);
        *folder = '/';
    }
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

int gear_hear_models(const char *dir, char names[][128], int max) {
    int count = 0, n = 0;
    char **found = SDL_GlobDirectory(dir, "parakeet*.gguf", 0, &count);
    if (max > 0) SDL_strlcpy(names[n++], files[GEAR_MODEL_HEAR].name, 128);
    for (int i = 0; found && i < count && n < max; i++)
        if (SDL_strcmp(found[i], files[GEAR_MODEL_HEAR].name)) SDL_strlcpy(names[n++], found[i], 128);
    SDL_free(found);
    // The rest in name order, so switching goes round them the same way each time.
    for (int i = 1; i < n; i++)
        for (int j = i; j > 1 && SDL_strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[128];
            SDL_memcpy(t, names[j], 128), SDL_memcpy(names[j], names[j - 1], 128), SDL_memcpy(names[j - 1], t, 128);
        }
    return n;
}

void gear_hear_model_remember(const char *dir, const char *name) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s/hear-model.txt", dir);
    SDL_SaveFile(path, name, SDL_strlen(name));
}

void gear_hear_model_pick(const char *dir, const char *choice, char *out, size_t size, char *name, size_t name_size) {
    char remembered[256] = "", path[1200];
    if (!choice || !*choice) {
        SDL_snprintf(path, sizeof path, "%s/hear-model.txt", dir);
        size_t n = 0;
        char *text = SDL_LoadFile(path, &n);
        if (text) SDL_strlcpy(remembered, text, SDL_min(n + 1, sizeof remembered)), SDL_free(text);
        // One that has since been deleted gives way to the default.
        SDL_snprintf(path, sizeof path, "%s/%s", dir, remembered);
        if (remembered[0] && file_size(path) > 0) choice = remembered;
    }
    if (choice && *choice && (SDL_strchr(choice, '/') || SDL_strchr(choice, '\\'))) {
        SDL_strlcpy(out, choice, size);
        const char *a = SDL_strrchr(choice, '/'), *b = SDL_strrchr(choice, '\\');
        SDL_strlcpy(name, (a > b ? a : b) + 1, name_size);
        return;
    }
    const char *file = choice && *choice ? choice : files[GEAR_MODEL_HEAR].name;
    SDL_snprintf(out, size, "%s/%s", dir, file);
    SDL_strlcpy(name, file, name_size);
}

double gear_countdown_remembered(const char *dir, double fallback) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s/countdown.txt", dir);
    char *text = SDL_LoadFile(path, NULL);
    double s = text ? SDL_atof(text) : 0;
    SDL_free(text);
    return s > 0 ? s : fallback;
}

void gear_countdown_remember(const char *dir, double seconds) {
    char path[1200], text[32];
    SDL_snprintf(path, sizeof path, "%s/countdown.txt", dir);
    SDL_snprintf(text, sizeof text, "%g", seconds);
    SDL_SaveFile(path, text, SDL_strlen(text));
}

float gear_volume_remembered(const char *dir, float fallback) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s/volume.txt", dir);
    char *text = SDL_LoadFile(path, NULL);
    float v = text ? SDL_clamp((float)SDL_atof(text), 0, 2) : fallback;
    SDL_free(text);
    return v;
}

void gear_volume_remember(const char *dir, float volume) {
    char path[1200], text[32];
    SDL_snprintf(path, sizeof path, "%s/volume.txt", dir);
    SDL_snprintf(text, sizeof text, "%.2f", volume);
    SDL_SaveFile(path, text, SDL_strlen(text));
}

bool gear_target_remembered(const char *dir, char *name, size_t name_size, char *host, size_t host_size) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s/target.txt", dir);
    char *text = SDL_LoadFile(path, NULL);
    if (!text) return false;
    char *line2 = SDL_strchr(text, '\n');
    if (line2) *line2++ = 0;
    SDL_strlcpy(name, text, name_size);
    SDL_strlcpy(host, line2 ? line2 : "", host_size);
    host[strcspn(host, "\r\n")] = 0;
    SDL_free(text);
    return name[0] != 0;
}

void gear_target_remember(const char *dir, const char *name, const char *host) {
    char path[1200], text[600];
    SDL_snprintf(path, sizeof path, "%s/target.txt", dir);
    if (!name || !*name) { SDL_RemovePath(path); return; }
    SDL_snprintf(text, sizeof text, "%s\n%s\n", name, host ? host : "");
    SDL_SaveFile(path, text, SDL_strlen(text));
}
