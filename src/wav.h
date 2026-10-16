// WAV files for the test modes and tests: any WAV SDL reads, converted to mono float at the rate
// asked for, and 16-bit PCM written out.
#pragma once
#include <stdbool.h>

// The file's samples as mono float at `rate`, or NULL; *n is the sample count. SDL_free the result.
float *gear_wav_read(const char *path, int rate, int *n);
// Writes `channels` interleaved float channels of `frames` frames as 16-bit PCM.
bool gear_wav_write(const char *path, const float *samples, int frames, int channels, int rate);
