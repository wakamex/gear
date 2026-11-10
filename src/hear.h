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
// What the recognizer's own scores say of a piece, each a mean over the word pieces it wrote: the
// chosen one's score before any softmax, its margin over writing nothing (blank) and over the next best
// word piece, the log-sum-exp of all their scores, the chosen one's probability, and the recognizer's
// confidence (one minus the entropy of that distribution, normalized). `tokens` is how many it wrote, 0
// when the model gives no such scores.
typedef struct { int tokens; float raw, blank, second, energy, pmax, conf; } gear_hear_scores;
// The next transcribed piece's text, oldest first, or NULL when none is ready. SDL_free it. Its scores
// go to `scores` when that is not NULL.
char *gear_hear_take(gear_hear *h, gear_hear_scores *scores);
int gear_hear_pending(gear_hear *h);  // pieces queued or being transcribed
double gear_hear_last_seconds(gear_hear *h);  // how long the last piece took to transcribe
// The device the model runs on, as transcribe.cpp names it: "vulkan", "cpu", ...
const char *gear_hear_device(gear_hear *h);
