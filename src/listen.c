#include "listen.h"
#include <SDL3/SDL.h>

struct gear_listen {
    gear_listen_params p;
    float *ring;           // the last preroll_frames frames, for the start of a piece
    int ring_next, ring_count;
    float *piece;          // the piece being collected
    int piece_frames, piece_cap;
    int above, below;      // frames in a row above `on`, and below `off` inside a piece
    bool in_piece;
};

gear_listen_params gear_listen_defaults(void) {
    // WebRTC's gain takes 0.95 for speech; a lower start catches soft onsets and the preroll keeps
    // what came before. A pause of 0.5 s ends a piece, well inside the message's countdown, and 30 s
    // keeps one recognition short.
    return (gear_listen_params){ .on = 0.8f, .off = 0.5f, .start_frames = 3, .pause_frames = 50, .preroll_frames = 30, .max_frames = 3000 };
}

gear_listen *gear_listen_new(const gear_listen_params *p) {
    gear_listen *l = SDL_calloc(1, sizeof *l);
    l->p = *p;
    l->ring = SDL_calloc((size_t)(p->preroll_frames + 1) * GEAR_FRAME, sizeof(float));
    return l;
}

void gear_listen_free(gear_listen *l) {
    if (!l) return;
    SDL_free(l->ring);
    SDL_free(l->piece);
    SDL_free(l);
}

static void append(gear_listen *l, const float *frame) {
    if (l->piece_frames == l->piece_cap) {
        l->piece_cap = l->piece_cap ? l->piece_cap * 2 : 512;
        l->piece = SDL_realloc(l->piece, (size_t)l->piece_cap * GEAR_FRAME * sizeof(float));
    }
    SDL_memcpy(l->piece + (size_t)l->piece_frames * GEAR_FRAME, frame, GEAR_FRAME * sizeof(float));
    l->piece_frames++;
}

gear_listen_event gear_listen_frame(gear_listen *l, const float *frame, float speech) {
    if (!l->in_piece) {
        int slots = l->p.preroll_frames + 1;
        SDL_memcpy(l->ring + (size_t)l->ring_next * GEAR_FRAME, frame, GEAR_FRAME * sizeof(float));
        l->ring_next = (l->ring_next + 1) % slots;
        if (l->ring_count < slots) l->ring_count++;
        l->above = speech >= l->p.on ? l->above + 1 : 0;
        if (l->above < l->p.start_frames) return GEAR_LISTEN_NONE;
        // The piece opens with the frames kept from before, oldest first, this one last.
        l->piece_frames = 0;
        for (int i = 0; i < l->ring_count; i++) append(l, l->ring + (size_t)((l->ring_next - l->ring_count + i + slots) % slots) * GEAR_FRAME);
        l->ring_count = 0;
        l->in_piece = true;
        l->below = 0;
        return GEAR_LISTEN_START;
    }
    append(l, frame);
    l->below = speech < l->p.off ? l->below + 1 : 0;
    if (l->below < l->p.pause_frames && l->piece_frames < l->p.max_frames) return GEAR_LISTEN_NONE;
    l->in_piece = false;
    l->above = 0;
    return GEAR_LISTEN_END;
}

const float *gear_listen_piece(const gear_listen *l, int *samples) {
    *samples = l->piece_frames * GEAR_FRAME;
    return l->piece;
}

bool gear_listen_in_piece(const gear_listen *l) { return l->in_piece; }
