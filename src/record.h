// With GEAR_DEBUG, everything the listener works with, written as it goes: one 16 kHz WAV of three
// channels on the same clock, the microphone as recorded, what the speakers played (the echo
// canceller's reference; silent when the speakers are not recorded) and the microphone as cleaned.
// A file holds 5 minutes, named for the local time its first sample was heard (2026-11-09-150500.wav),
// so a moment in heard.jsonl can be found in it to the sample; the newest hour is kept.
#pragma once

typedef struct gear_record gear_record;

gear_record *gear_record_open(const char *dir);  // made when missing; NULL when it cannot be
void gear_record_frame(gear_record *r, const float *mic, const float *played, const float *clean);  // GEAR_FRAME each; played may be NULL
void gear_record_close(gear_record *r);
