// Telling speech from everything else in the microphone's sound: Silero VAD (hearsay, through
// transcribe.cpp), which scores each 32 ms of 16 kHz audio with the probability that it is speech.
// It runs on the CPU, about 0.06 ms of work for each 32 ms.
#pragma once
#include <stddef.h>

typedef struct gear_vad gear_vad;

// Loads the model (a GGUF file). NULL with the reason in `error`.
gear_vad *gear_vad_open(const char *model, char *error, size_t error_size);
void gear_vad_free(gear_vad *v);
// Takes the next 10 ms frame (GEAR_FRAME samples); the speech probability of the last complete
// 32 ms window, 0 before the first.
float gear_vad_frame(gear_vad *v, const float *frame);
