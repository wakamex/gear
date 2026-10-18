// gear's window: the spoken loop on the microphone and speakers, drawn with gesso's gs_ui in the
// toolcore-native style. One column: a state line with the target and a
// microphone meter, the message being said with its countdown, the reply being read, and a status bar.
#pragma once
#include <stdbool.h>

typedef struct {
    const char *models;   // the models folder
    const char *to;       // the agent messages go to, or NULL to read each message back
    double countdown;
    int hear_threads, say_threads;
    bool no_echo;
    bool no_keep;         // do not keep what is heard (see gear_pipeline_keep)
    const char *shot;     // write a picture of the window to this PNG once it has drawn, and exit
    bool demo;            // show a made-up exchange instead of running the loop (no models needed)
    const char *hear_model;  // a recognition model file or path; NULL for the last chosen
} gear_window_options;

// Runs until the window closes; the exit code. GEAR_UPDATED when a newer build was put in place
// and the window ended, idle, for it to be started.
int gear_window(const gear_window_options *o);
enum { GEAR_UPDATED = 75 };
