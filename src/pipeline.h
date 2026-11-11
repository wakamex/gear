// The spoken loop's logic, apart from any device: microphone frames in, a message out. Each frame has
// the echo taken out by gs_voice and is judged by Silero (gear_vad); the listener cuts the frames into pieces of
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
#include "vad.h"

typedef struct gear_pipeline gear_pipeline;

// `say` may be NULL (nothing to interrupt). The pipeline does not own its parts.
gear_pipeline *gear_pipeline_new(gs_voice *voice, gear_vad *vad, gear_hear *hear, gear_say *say, const gear_listen_params *listen, double countdown);
void gear_pipeline_free(gear_pipeline *p);
// Keeps what is heard in `dir`, for training a recogniser on the user's own speech later: each piece
// as a 16 kHz WAV file in a folder for the day, and in dir/heard.jsonl a line per piece (its file,
// the text `model` heard in it, the seconds that took, its message, and for a piece with words the
// inputs and the score of speech.h) and a line per message saying whether it was sent, sent now or
// dropped. NULL keeps nothing (the default).
void gear_pipeline_keep(gear_pipeline *p, const char *dir, const char *model);
// Another recogniser from here on, called `model` in what is kept. The old one is the caller's to close.
void gear_pipeline_set_hear(gear_pipeline *p, gear_hear *hear, const char *model);
// One microphone frame of GEAR_FRAME samples at 16 kHz, cleaned in place, and what the speakers played
// in the same 10 ms (NULL when they are not recorded; what `say` played then stands for them, where it
// keeps that). When a piece of speech ends while the speakers played something, their sound over the
// same stretch is transcribed after it, and the piece's text is dropped when at least three in four of
// its words are in theirs: a reply, a video or a call heard through the speakers, which the echo
// canceller let through, rather than the user. Its text joins the message only once both are in. A
// piece whose words score as another sound of the voice than speech (speech.h) is dropped as well.
void gear_pipeline_frame(gear_pipeline *p, float *frame, const float *played);
// Collects finished transcriptions. Returns the message when its countdown has run out (SDL_free it),
// and starts the next one; NULL otherwise.
char *gear_pipeline_poll(gear_pipeline *p);
const char *gear_pipeline_text(const gear_pipeline *p);  // the message so far
// With drafts on, the piece being spoken is transcribed again every 0.25 s while it lasts, and its
// words so far are gear_pipeline_draft until the piece's own text joins the message ("" when there
// are none). A draft is not checked against the speakers or scored as speech, as a piece is. Off by
// default; on the CPU a draft can hold up a piece's text by its own running time.
void gear_pipeline_set_drafts(gear_pipeline *p, bool on);
const char *gear_pipeline_draft(const gear_pipeline *p);
double gear_pipeline_countdown(const gear_pipeline *p);  // seconds left before sending, < 0 when not counting
void gear_pipeline_set_countdown(gear_pipeline *p, double seconds);  // from the next frame on
bool gear_pipeline_hearing(const gear_pipeline *p);      // in a piece of speech now
void gear_pipeline_cancel(gear_pipeline *p);             // drops the message
// The message as it stands, sent now rather than when the countdown ends (the Enter key), or NULL
// when there is none yet. SDL_free it.
char *gear_pipeline_take(gear_pipeline *p);
bool gear_pipeline_pending(const gear_pipeline *p);      // pieces heard but not yet transcribed
double gear_pipeline_time(const gear_pipeline *p);       // seconds of microphone audio so far
int gear_pipeline_interruptions(const gear_pipeline *p); // replies stopped by talking
