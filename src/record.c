#include "record.h"
#include <SDL3/SDL.h>
#include "listen.h"

enum { FILE_FRAMES = 5 * 60 * 100, KEEP_FILES = 12, CHANNELS = 3 };

struct gear_record {
    char dir[1024];
    SDL_IOStream *io;
    int frames;  // in the open file
};

static void header(SDL_IOStream *io, Uint32 data) {
    Uint8 h[44] = "RIFF....WAVEfmt ";
    Uint32 rate = GEAR_RATE, bytes_per_sec = GEAR_RATE * CHANNELS * 2;
    SDL_memcpy(h + 4, &(Uint32){ 36 + data }, 4);
    SDL_memcpy(h + 16, &(Uint32){ 16 }, 4);
    SDL_memcpy(h + 20, &(Uint16){ 1 }, 2);
    SDL_memcpy(h + 22, &(Uint16){ CHANNELS }, 2);
    SDL_memcpy(h + 24, &rate, 4);
    SDL_memcpy(h + 28, &bytes_per_sec, 4);
    SDL_memcpy(h + 32, &(Uint16){ CHANNELS * 2 }, 2);
    SDL_memcpy(h + 34, &(Uint16){ 16 }, 2);
    SDL_memcpy(h + 36, "data", 4);
    SDL_memcpy(h + 40, &data, 4);
    SDL_SeekIO(io, 0, SDL_IO_SEEK_SET);
    SDL_WriteIO(io, h, sizeof h);
}

static void finish(gear_record *r) {
    if (!r->io) return;
    header(r->io, (Uint32)r->frames * GEAR_FRAME * CHANNELS * 2);  // the sizes, now known
    SDL_CloseIO(r->io);
    r->io = NULL;
}

// A new file named for now, and the oldest beyond the newest hour removed.
static void start(gear_record *r) {
    SDL_Time t;
    SDL_DateTime d;
    SDL_GetCurrentTime(&t);
    SDL_TimeToDateTime(t, &d, true);
    char path[1100];
    SDL_snprintf(path, sizeof path, "%s/%04d-%02d-%02d-%02d%02d%02d.wav", r->dir, d.year, d.month, d.day, d.hour, d.minute, d.second);
    r->io = SDL_IOFromFile(path, "wb");
    r->frames = 0;
    if (r->io) header(r->io, 0);
    int count = 0;
    char **names = SDL_GlobDirectory(r->dir, "*.wav", 0, &count);
    for (int i = 0; names && i + KEEP_FILES < count; i++) {
        // (the names sort by time; SDL lists them in no set order)
        int oldest = i;
        for (int j = i + 1; j < count; j++) if (SDL_strcmp(names[j], names[oldest]) < 0) oldest = j;
        char *swap = names[i];
        names[i] = names[oldest], names[oldest] = swap;
        SDL_snprintf(path, sizeof path, "%s/%s", r->dir, names[i]);
        SDL_RemovePath(path);
    }
    SDL_free(names);
}

gear_record *gear_record_open(const char *dir) {
    if (!SDL_CreateDirectory(dir)) return NULL;
    gear_record *r = SDL_calloc(1, sizeof *r);
    SDL_strlcpy(r->dir, dir, sizeof r->dir);
    return r;
}

static Sint16 sample(float x) { return (Sint16)SDL_clamp(x * 32767.0f, -32768.0f, 32767.0f); }

void gear_record_frame(gear_record *r, const float *mic, const float *played, const float *clean) {
    if (!r) return;
    if (!r->io || r->frames == FILE_FRAMES) finish(r), start(r);
    if (!r->io) return;
    Sint16 out[GEAR_FRAME * CHANNELS];
    for (int i = 0; i < GEAR_FRAME; i++) {
        out[i * 3] = sample(mic[i]);
        out[i * 3 + 1] = played ? sample(played[i]) : 0;
        out[i * 3 + 2] = sample(clean[i]);
    }
    SDL_WriteIO(r->io, out, sizeof out);
    r->frames++;
}

void gear_record_close(gear_record *r) {
    if (!r) return;
    finish(r);
    SDL_free(r);
}
