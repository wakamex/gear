#include "speech.h"
#include <SDL3/SDL.h>
#include "speech_model.h"

void gear_speech_frame(gear_speech_heard *h, float silero, float second) {
    h->s50 += silero >= 0.5f, h->s80 += silero >= 0.8f, h->s90 += silero >= 0.9f, h->s95 += silero >= 0.95f, h->s99 += silero >= 0.99f;
    h->r50 += second >= 0.5f, h->r90 += second >= 0.9f, h->r95 += second >= 0.95f;
    h->speak = SDL_max(h->speak, silero), h->rpeak = SDL_max(h->rpeak, second);
    if (silero >= 0.5f) h->ssum += silero, h->rsum += second;
}

void gear_speech_inputs(const gear_speech_heard *h, const gear_hear_scores *s, float in[GEAR_SPEECH_INPUTS]) {
    float talk = (float)SDL_max(h->s50, 1);
    const float all[GEAR_SPEECH_INPUTS] = {
        (float)h->s95 / talk, (float)h->s99 / talk, (float)h->s90 / talk, (float)h->s80 / talk, h->speak, (float)(h->ssum / talk),
        s->raw, s->blank, s->second, s->energy, s->pmax, s->conf,
        (float)h->r50 / talk, (float)h->r90 / talk, (float)h->r95 / talk, h->rpeak, (float)(h->rsum / talk),
    };
    SDL_memcpy(in, all, sizeof all);
}

float gear_speech_score(const float in[GEAR_SPEECH_INPUTS]) {
    double sum = 0;
    for (size_t t = 0; t < SDL_arraysize(speech_roots); t++) {
        int i = speech_roots[t];
        while (speech_nodes[i].input >= 0) i = (double)in[speech_nodes[i].input] <= speech_nodes[i].value ? speech_nodes[i].low : speech_nodes[i].high;
        sum += speech_nodes[i].value;
    }
    return (float)(1 / (1 + SDL_exp(-sum)));
}

float gear_speech_cut(void) { return speech_cut; }

float gear_speech_checked(void) {
    float worst = 0;
    for (size_t i = 0; i < SDL_arraysize(speech_checks); i++) worst = SDL_max(worst, SDL_fabsf(gear_speech_score(speech_checks[i].in) - speech_checks[i].score));
    return worst;
}
