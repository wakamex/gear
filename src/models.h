// The model files gear runs on, pinned by commit with their sizes and SHA-256 sums, and their
// download: each streamed to a .part beside its place, resumed where a broken download left off,
// hashed as it arrives, and moved into place only when its size and checksum match.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum { GEAR_MODEL_HEAR, GEAR_MODEL_SAY, GEAR_MODEL_VOICE, GEAR_MODEL_COUNT } gear_model;

typedef struct {
    const char *name;    // the file's name in the models folder
    const char *url;     // at a pinned commit
    long long bytes;
    const char *sha256;
    const char *about;   // what it is and its license, for the user
} gear_model_file;

const gear_model_file *gear_model_info(gear_model m);
// The folder the models are kept in: GEAR_MODELS when set, else a "models" folder in the user's data
// folder for gear. Made when missing. NULL when there is none.
const char *gear_models_dir(void);
void gear_model_path(const char *dir, gear_model m, char *out, size_t size);
// Whether every file is in place at its size. The checksum is checked when a file arrives.
bool gear_models_present(const char *dir);

// received and total in bytes over all the files; eta is the seconds left, or < 0 when not known yet.
typedef void gear_fetch_progress(void *user, long long received, long long total, double eta);
// Downloads what is missing. Waits for the network: call it from a worker or a command-line mode.
// Setting *stop gives it up. False with the reason in `error` ("stopped" after a stop).
bool gear_models_fetch(const char *dir, SDL_AtomicInt *stop, gear_fetch_progress *progress, void *user, char *error, size_t error_size);
