// Speech synthesis on a thread of its own, played as it is made. Text queued with gear_say_speak is
// synthesized by Pocket TTS sentence by sentence; each chunk of audio goes into a gs_stream that is a
// source in gs_mix, so the first words play while the rest is still being made. gear_say_stop drops
// what is queued, ends the synthesis in progress at its next chunk and silences what is waiting to
// play, for when the user starts talking over a reply.
//
// What plays is also handed to gs_voice's echo canceller, 10 ms at a time at 16 kHz, so that with
// loudspeakers the microphone does not hear the reply as the user talking.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "gs_voice.h"

typedef struct gear_say gear_say;

// Loads the Pocket model and voice on the calling thread and adds the player to gs_mix, whose rate
// (gs_mix_rate) must be a multiple of 16 kHz. `echo` may be NULL. NULL with the reason in `error`.
gear_say *gear_say_open(const char *model, const char *voice, int threads, gs_voice *echo, char *error, size_t error_size);
void gear_say_close(gear_say *s);
void gear_say_speak(gear_say *s, const char *text);  // queued after anything already speaking
void gear_say_stop(gear_say *s);
bool gear_say_busy(gear_say *s);  // synthesizing, or audio still to play
// Seconds from the start of the last synthesis to its first audio, and the seconds of audio it made.
void gear_say_last(gear_say *s, double *first_audio, double *audio);
