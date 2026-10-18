#include "update.h"
#include <SDL3/SDL.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "gs_ed25519.h"
#include "gs_http.h"
#include "gs_sha256.h"

// Where updates are published, unless GEAR_UPDATE_FROM names another.
#define SERVER "http://136.243.110.49:8071"
// The publisher's key; tools/publish-update signs with its private half.
static const Uint8 publisher[32] = {
    0x45, 0xbb, 0xa1, 0x28, 0xcb, 0x43, 0x4d, 0xe9, 0xe0, 0x8d, 0x0a, 0xc5, 0x99, 0x0a, 0x27, 0x6b,
    0x9d, 0x6c, 0xf0, 0x33, 0x08, 0xde, 0x1c, 0xad, 0x1b, 0x07, 0x9a, 0xa5, 0xcf, 0xde, 0x20, 0x7a,
};
enum { EVERY_MS = 60000, MAX_FILES = 8 };

struct gear_update {
    char folder[1024], server[256];
    SDL_Semaphore *stop;
    SDL_AtomicInt stopping, ready;
    SDL_Thread *thread;
};

const char *gear_update_build(void) {
#ifdef GEAR_BUILD
    return GEAR_BUILD;
#else
    return NULL;
#endif
}

typedef struct {
    char name[64], hash[65];
    long long size;
} file;

static bool hex_to(const char *hex, Uint8 *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + d;
        }
        out[i] = (Uint8)v;
    }
    return true;
}

// Reads a manifest given its signature in hex: its build and files, or false with the reason.
static bool manifest(const char *text, size_t len, const char *signature, char *build, size_t build_size, file *files, int *count, char *error, size_t error_size) {
    Uint8 sig[64];
    while (*signature == ' ' || *signature == '\n') signature++;
    if (strspn(signature, "0123456789abcdefABCDEF") != 128 || !hex_to(signature, sig, 64)) {
        SDL_strlcpy(error, "the signature is not 64 bytes in hex", error_size);
        return false;
    }
    if (!gs_ed25519_verify(sig, publisher, text, len)) {
        SDL_strlcpy(error, "the signature does not match the manifest", error_size);
        return false;
    }
    build[0] = 0, *count = 0;
    for (const char *line = text; line < text + len;) {
        const char *end = memchr(line, '\n', (size_t)(text + len - line));
        if (!end) end = text + len;
        char l[256], name[256], hash[256];
        long long size;
        SDL_strlcpy(l, line, SDL_min((size_t)(end - line) + 1, sizeof l));
        if (!SDL_strncmp(l, "build ", 6)) SDL_strlcpy(build, l + 6, build_size);
        else if (SDL_sscanf(l, "file %255s %255s %lld", name, hash, &size) == 3) {
            // (a name is a file beside the program, never a path elsewhere)
            if (!name[0] || strpbrk(name, "/\\:") || name[0] == '.' || SDL_strlen(name) >= sizeof files->name ||
                SDL_strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64) {
                SDL_snprintf(error, error_size, "the manifest names a file it may not: %s", name);
                return false;
            }
            if (*count == MAX_FILES) { SDL_strlcpy(error, "the manifest names too many files", error_size); return false; }
            file *f = &files[(*count)++];
            SDL_strlcpy(f->name, name, sizeof f->name), SDL_strlcpy(f->hash, hash, sizeof f->hash), f->size = size;
        }
        line = end + 1;
    }
    if (!build[0]) { SDL_strlcpy(error, "the manifest names no build", error_size); return false; }
    return true;
}

static void file_hash(const char *path, char hex[65]) {
    hex[0] = 0;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return;
    gs_sha256 h;
    gs_sha256_init(&h);
    static Uint8 buf[1 << 16];
    for (size_t n; (n = SDL_ReadIO(io, buf, sizeof buf)) > 0;) gs_sha256_update(&h, buf, n);
    SDL_CloseIO(io);
    Uint8 digest[32];
    gs_sha256_final(&h, digest);
    gs_sha256_hex(digest, hex);
}

typedef struct {
    SDL_IOStream *io;
    gs_sha256 hash;
    long long have;
    bool failed;
} sink;

static bool take(void *user, const Uint8 *data, size_t len) {
    sink *s = user;
    if (SDL_WriteIO(s->io, data, len) != len) { s->failed = true; return false; }
    gs_sha256_update(&s->hash, data, len);
    s->have += (long long)len;
    return true;
}

// Downloads one file of the build to `to`, checked against the manifest.
static bool fetch(gear_update *u, const file *f, const char *to, char *error, size_t error_size) {
    char url[512];
    SDL_snprintf(url, sizeof url, "%s/files/%s", u->server, f->hash);
    sink s = { .io = SDL_IOFromFile(to, "wb") };
    if (!s.io) { SDL_snprintf(error, error_size, "%s: %s", to, SDL_GetError()); return false; }
    gs_sha256_init(&s.hash);
    gs_http_request r = { .url = url, .agent = "gear" };
    int status = gs_http_stream(&r, 0, take, &s, &u->stopping);
    SDL_CloseIO(s.io);
    Uint8 digest[32];
    char hex[65];
    gs_sha256_final(&s.hash, digest);
    gs_sha256_hex(digest, hex);
    if (status == 200 && !s.failed && s.have == f->size && !SDL_strcmp(hex, f->hash)) return true;
    SDL_RemovePath(to);
    if (status != 200) SDL_snprintf(error, error_size, "%s: HTTP %d", f->name, status);
    else SDL_snprintf(error, error_size, "%s did not arrive as the manifest describes it", f->name);
    return false;
}

// Looks once. True when a newer build has been put in place.
static bool look(gear_update *u, char *error, size_t error_size) {
    char url[512], *text = NULL, *signature = NULL;
    size_t len = 0, sig_len = 0;
    SDL_snprintf(url, sizeof url, "%s/manifest.txt", u->server);
    gs_http_request r = { .url = url, .agent = "gear" };
    int status = gs_http_fetch(&r, &text, &len);
    SDL_snprintf(url, sizeof url, "%s/manifest.sig", u->server);
    int sig_status = status == 200 ? gs_http_fetch(&r, &signature, &sig_len) : 0;
    char build[64];
    file files[MAX_FILES];
    int count = 0;
    bool ok = status == 200 && sig_status == 200;
    if (!ok) SDL_snprintf(error, error_size, "the update server answered %d", status == 200 ? sig_status : status);
    else ok = manifest(text, len, signature, build, sizeof build, files, &count, error, error_size);
    SDL_free(text);
    SDL_free(signature);
    if (!ok || !SDL_strcmp(build, gear_update_build())) return false;
    // Every file that differs is fetched and checked before any is put in place.
    bool fetched[MAX_FILES] = { 0 }, any = false;
    char here[1200], next[1210], old[1210];
    for (int i = 0; i < count; i++) {
        char hex[65];
        SDL_snprintf(here, sizeof here, "%s%s", u->folder, files[i].name);
        file_hash(here, hex);
        if (!SDL_strcmp(hex, files[i].hash)) continue;
        SDL_snprintf(next, sizeof next, "%s.new", here);
        if (!fetch(u, &files[i], next, error, error_size)) return false;
        fetched[i] = any = true;
    }
    // (files that all match already are this build, whatever it calls itself: nothing to start again for)
    if (!any) return false;
    for (int i = 0; i < count; i++) {
        if (!fetched[i]) continue;
        SDL_snprintf(here, sizeof here, "%s%s", u->folder, files[i].name);
        SDL_snprintf(next, sizeof next, "%s.new", here);
        SDL_snprintf(old, sizeof old, "%s.old", here);
        SDL_RemovePath(old);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(here, &info) && !SDL_RenamePath(here, old)) {
            SDL_snprintf(error, error_size, "%s: %s", here, SDL_GetError());
            return false;
        }
        if (!SDL_RenamePath(next, here)) {
            SDL_snprintf(error, error_size, "%s: %s", here, SDL_GetError());
            return false;
        }
#ifndef _WIN32
        // (with the old file's permissions, so that the program stays one that can be run)
        if (!SDL_strcmp(files[i].name, "gear")) chmod(here, 0755);
#endif
    }
    SDL_Log("gear: updated to build %s; it starts when gear is idle", build);
    return true;
}

static int run(void *user) {
    gear_update *u = user;
    char said[300] = "";
    do {
        char error[300] = "";
        if (look(u, error, sizeof error)) {
            SDL_SetAtomicInt(&u->ready, 1);
            return 0;
        }
        // (a failure is logged when it first happens, not every minute)
        if (error[0] && SDL_strcmp(error, said)) SDL_Log("gear: looking for an update: %s", error);
        SDL_strlcpy(said, error, sizeof said);
    } while (!SDL_WaitSemaphoreTimeout(u->stop, EVERY_MS));
    return 0;
}

gear_update *gear_update_start(const char *folder) {
    if (!gear_update_build() || !folder) return NULL;
    gear_update *u = SDL_calloc(1, sizeof *u);
    SDL_strlcpy(u->folder, folder, sizeof u->folder);
    const char *from = SDL_getenv("GEAR_UPDATE_FROM");
    SDL_strlcpy(u->server, from && *from ? from : SERVER, sizeof u->server);
    // (what was renamed aside by the last update is no longer running, and goes)
    int count = 0;
    char **old = SDL_GlobDirectory(folder, "*.old", 0, &count);
    for (int i = 0; old && i < count; i++) {
        char path[1200];
        SDL_snprintf(path, sizeof path, "%s%s", folder, old[i]);
        SDL_RemovePath(path);
    }
    SDL_free(old);
    u->stop = SDL_CreateSemaphore(0);
    u->thread = SDL_CreateThread(run, "gear-update", u);
    return u;
}

bool gear_update_ready(gear_update *u) { return u && SDL_GetAtomicInt(&u->ready); }

void gear_update_free(gear_update *u) {
    if (!u) return;
    SDL_SetAtomicInt(&u->stopping, 1);
    SDL_SignalSemaphore(u->stop);
    SDL_WaitThread(u->thread, NULL);
    SDL_DestroySemaphore(u->stop);
    SDL_free(u);
}

bool gear_update_restart(const char *folder, int argc, char **argv) {
    char program[1100];
#ifdef _WIN32
    SDL_snprintf(program, sizeof program, "%sgear.exe", folder);
#else
    SDL_snprintf(program, sizeof program, "%sgear", folder);
#endif
    const char *args[72];
    int n = 0;
    args[n++] = program;
    for (int i = 1; i < argc && n < 71; i++) args[n++] = argv[i];
    args[n] = NULL;
    // (SDL's own process, not gs_http_spawn's, whose Windows job would end it with this one; in the
    // background, which on Windows starts the console program without a console window of its own)
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *next = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (!next) {
        SDL_Log("gear: the new build could not be started: %s", SDL_GetError());
        return false;
    }
    SDL_DestroyProcess(next);
    return true;
}
