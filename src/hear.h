// Speech recognition on a thread of its own: pieces of speech go in as the listener cuts them, their
// text comes out in the same order, so the window never waits on the recogniser.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct gear_hear gear_hear;

// Loads the Parakeet model (a GGUF file) on the calling thread. NULL with the reason in `error`.
gear_hear *gear_hear_open(const char *model, int threads, char *error, size_t error_size);
void gear_hear_close(gear_hear *h);  // waits for the piece being transcribed
// Queues a copy of `samples` (16 kHz mono) for transcription.
void gear_hear_submit(gear_hear *h, const float *samples, int n);
// The next transcribed piece's text, oldest first, or NULL when none is ready. SDL_free it.
char *gear_hear_take(gear_hear *h);
int gear_hear_pending(gear_hear *h);  // pieces queued or being transcribed
double gear_hear_last_seconds(gear_hear *h);  // how long the last piece took to transcribe
// The device the model runs on, as transcribe.cpp names it: "vulkan", "cpu", ...
const char *gear_hear_device(gear_hear *h);
