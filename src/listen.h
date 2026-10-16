// Cutting the microphone's 10 ms frames into pieces of speech, by the voice detector's answer for
// each frame. A piece starts once the detector has heard speech for a few frames in a row and keeps
// the audio from just before, so the first sound is not lost; it ends after a pause, or at a length
// limit. The pieces are what the recogniser transcribes; the message they make up is the pipeline's.
#pragma once
#include <stdbool.h>

enum { GEAR_FRAME = 160, GEAR_RATE = 16000 };  // 10 ms at 16 kHz, gs_voice's frame

typedef struct {
    float on, off;       // speech probability that starts speech, and below which a frame is pause
    int start_frames;    // frames in a row at or above `on` that start a piece
    int pause_frames;    // frames in a row below `off` that end it
    int preroll_frames;  // frames kept from before the start
    int max_frames;      // a piece longer than this is cut
} gear_listen_params;

gear_listen_params gear_listen_defaults(void);

typedef enum { GEAR_LISTEN_NONE, GEAR_LISTEN_START, GEAR_LISTEN_END } gear_listen_event;
typedef struct gear_listen gear_listen;

gear_listen *gear_listen_new(const gear_listen_params *p);
void gear_listen_free(gear_listen *l);
// One frame of GEAR_FRAME samples and the detector's probability for it. START when a piece begins;
// END when one is complete, its audio then readable with gear_listen_piece until the next frame.
gear_listen_event gear_listen_frame(gear_listen *l, const float *frame, float speech);
const float *gear_listen_piece(const gear_listen *l, int *samples);
bool gear_listen_in_piece(const gear_listen *l);
