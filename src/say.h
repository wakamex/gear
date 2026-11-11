// Speech synthesis on a thread of its own, played as it is made. Text queued with gear_say_speak is
// synthesized by Pocket TTS (hearsay's pockettts.cpp) sentence by sentence; each chunk of audio goes
// into a gs_stream that is a source in gs_mix, so the first words play while the rest is still
// being made. gear_say_stop drops what is queued, ends the synthesis in progress at its next chunk
// and silences what is waiting to play, for when the user starts talking over a reply.
//
// Where the microphone hears what plays (loudspeakers that are not recorded), what plays is also
// handed to gs_voice's echo canceller, 10 ms at a time at 16 kHz, so that the microphone does not hear
// the reply as the user talking, and kept for gear_say_played.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "gs_voice.h"

typedef struct gear_say gear_say;

// Loads the Pocket model and voice on the calling thread and adds the player to gs_mix, whose rate
// (gs_mix_rate) must be a multiple of 16 kHz. `echo` is the voice whose microphone hears what plays,
// or NULL (headphones, or speakers recorded another way). NULL with the reason in `error`.
gear_say *gear_say_open(const char *model, const char *voice, int threads, gs_voice *echo, char *error, size_t error_size);
void gear_say_close(gear_say *s);
void gear_say_speak(gear_say *s, const char *text);  // queued after anything already speaking
// Speaks in the voice file `voice` from the next line synthesized on; a line already being spoken
// keeps its voice. The old voice stays if the new one does not load.
void gear_say_set_voice(gear_say *s, const char *voice);
void gear_say_stop(gear_say *s);
bool gear_say_busy(gear_say *s);  // synthesizing, or audio still to play
// The last `n` samples played, at 16 kHz, into `out`; silence for any older than 31.9 s. False, with
// `out` untouched, when opened without `echo`.
bool gear_say_played(gear_say *s, float *out, int n);
// Seconds from the start of the last synthesis to its first audio, and the seconds of audio it made.
void gear_say_last(gear_say *s, double *first_audio, double *audio);
