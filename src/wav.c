#include "wav.h"
#include <SDL3/SDL.h>

float *gear_wav_read(const char *path, int rate, int *n) {
    SDL_AudioSpec spec;
    Uint8 *data = NULL, *out = NULL;
    Uint32 len = 0;
    int out_len = 0;
    *n = 0;
    if (!SDL_LoadWAV(path, &spec, &data, &len)) return NULL;
    const SDL_AudioSpec want = { SDL_AUDIO_F32, 1, rate };
    bool ok = SDL_ConvertAudioSamples(&spec, data, (int)len, &want, &out, &out_len);
    SDL_free(data);
    if (!ok) return NULL;
    *n = out_len / (int)sizeof(float);
    return (float *)out;
}

static void put16(Uint8 *p, Uint16 v) { p[0] = (Uint8)v; p[1] = (Uint8)(v >> 8); }
static void put32(Uint8 *p, Uint32 v) { put16(p, (Uint16)v); put16(p + 2, (Uint16)(v >> 16)); }

bool gear_wav_write(const char *path, const float *samples, int frames, int channels, int rate) {
    Uint32 bytes = (Uint32)frames * (Uint32)channels * 2;
    Uint8 *buf = SDL_malloc(44 + bytes);
    if (!buf) return false;
    SDL_memcpy(buf, "RIFF", 4); put32(buf + 4, 36 + bytes); SDL_memcpy(buf + 8, "WAVEfmt ", 8);
    put32(buf + 16, 16); put16(buf + 20, 1); put16(buf + 22, (Uint16)channels); put32(buf + 24, (Uint32)rate);
    put32(buf + 28, (Uint32)(rate * channels * 2)); put16(buf + 32, (Uint16)(channels * 2)); put16(buf + 34, 16);
    SDL_memcpy(buf + 36, "data", 4); put32(buf + 40, bytes);
    for (int i = 0; i < frames * channels; i++) {
        float v = samples[i] < -1 ? -1 : samples[i] > 1 ? 1 : samples[i];
        put16(buf + 44 + 2 * i, (Uint16)(Sint16)SDL_lroundf(v * 32767));
    }
    bool ok = SDL_SaveFile(path, buf, 44 + bytes);
    SDL_free(buf);
    return ok;
}
