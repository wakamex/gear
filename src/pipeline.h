// The spoken loop's logic, apart from any device: microphone frames in, a message out. Each frame is
// cleaned by gs_voice and judged by its voice detector; the listener cuts the frames into pieces of
// speech, the recogniser turns each piece into text on its own thread, and the pieces add up to the
// message. Once the user has stopped talking and every piece is transcribed, a countdown runs, and
// talking again stops it. When it runs out the message is ready to send. Starting to talk while a
// reply is being read stops the reading.
//
// Time is counted in microphone frames (10 ms each), so a file played through the pipeline behaves as
// the same speech would live.
#pragma once
#include <stdbool.h>
#include "gs_voice.h"
#include "hear.h"
#include "listen.h"
#include "say.h"

typedef struct gear_pipeline gear_pipeline;

// `say` may be NULL (nothing to interrupt). The pipeline does not own its parts.
gear_pipeline *gear_pipeline_new(gs_voice *voice, gear_hear *hear, gear_say *say, const gear_listen_params *listen, double countdown);
void gear_pipeline_free(gear_pipeline *p);
// One microphone frame of GEAR_FRAME samples at 16 kHz, cleaned in place.
void gear_pipeline_frame(gear_pipeline *p, float *frame);
// Collects finished transcriptions. Returns the message when its countdown has run out (SDL_free it),
// and starts the next one; NULL otherwise.
char *gear_pipeline_poll(gear_pipeline *p);
const char *gear_pipeline_text(const gear_pipeline *p);  // the message so far
double gear_pipeline_countdown(const gear_pipeline *p);  // seconds left before sending, < 0 when not counting
bool gear_pipeline_hearing(const gear_pipeline *p);      // in a piece of speech now
void gear_pipeline_cancel(gear_pipeline *p);             // drops the message
double gear_pipeline_time(const gear_pipeline *p);       // seconds of microphone audio so far
int gear_pipeline_interruptions(const gear_pipeline *p); // replies stopped by talking
