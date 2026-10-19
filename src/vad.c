#include "vad.h"
#include <SDL3/SDL.h>
#include "listen.h"
#include "transcribe.h"
#include "transcribe/vad.h"

struct gear_vad {
    struct transcribe_model *model;
    struct transcribe_vad_session *session;
    float last;
};

gear_vad *gear_vad_open(const char *model, char *error, size_t error_size) {
    struct transcribe_model_load_params lp;
    transcribe_model_load_params_init(&lp);
    lp.backend = TRANSCRIBE_BACKEND_CPU;  // as Silero computes it; a GPU's half precision changes the scores
    gear_vad *v = SDL_calloc(1, sizeof *v);
    transcribe_status st = transcribe_model_load_file(model, &lp, &v->model);
    if (st == TRANSCRIBE_OK) {
        struct transcribe_vad_session_params sp;
        transcribe_vad_session_params_init(&sp);
        sp.n_threads = 1;
        st = transcribe_vad_session_init(v->model, &sp, &v->session);
    }
    if (st != TRANSCRIBE_OK) {
        SDL_snprintf(error, error_size, "Could not load %s: %s", model, transcribe_status_string(st));
        gear_vad_free(v);
        return NULL;
    }
    return v;
}

void gear_vad_free(gear_vad *v) {
    if (!v) return;
    transcribe_vad_session_free(v->session);
    transcribe_model_free(v->model);
    SDL_free(v);
}

float gear_vad_frame(gear_vad *v, const float *frame) {
    if (transcribe_vad_stream_feed(v->session, frame, GEAR_FRAME) != TRANSCRIBE_OK) return v->last;
    struct transcribe_vad_result r;
    transcribe_vad_result_init(&r);
    if (transcribe_vad_get_result(v->session, &r) == TRANSCRIBE_OK && r.n_probs > 0) v->last = transcribe_vad_probs(v->session)[r.n_probs - 1];
    return v->last;
}
