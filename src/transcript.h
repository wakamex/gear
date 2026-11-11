// The messages sent, for the window to show above the one being said: kept one JSON line each in a
// file ({"at":"2026-11-11 16:30","to":"agent","text":"..."}, local time), so they outlast a restart.
#pragma once

typedef struct { char *text; char at[24]; char to[96]; } gear_sent;
typedef struct gear_transcript gear_transcript;

// Loads the newest `keep` messages of the file at `path`; one that does not exist yet is empty.
gear_transcript *gear_transcript_open(const char *path, int keep);
void gear_transcript_close(gear_transcript *t);
// A message sent now to `to` ("" for none): added to the file and to the newest kept.
void gear_transcript_add(gear_transcript *t, const char *text, const char *to);
int gear_transcript_count(const gear_transcript *t);
const gear_sent *gear_transcript_at(const gear_transcript *t, int i);  // oldest first
