#include "transcript.h"
#include <SDL3/SDL.h>
#include "gs_json.h"

struct gear_transcript {
    char path[1100];
    gear_sent *sent;  // the newest `keep`, oldest first
    int count, keep;
};

static void push(gear_transcript *t, const char *text, const char *at, const char *to) {
    if (t->count == t->keep) {
        SDL_free(t->sent[0].text);
        SDL_memmove(t->sent, t->sent + 1, (size_t)(t->keep - 1) * sizeof *t->sent);
        t->count--;
    }
    gear_sent *s = &t->sent[t->count++];
    s->text = SDL_strdup(text);
    SDL_strlcpy(s->at, at, sizeof s->at);
    SDL_strlcpy(s->to, to, sizeof s->to);
}

gear_transcript *gear_transcript_open(const char *path, int keep) {
    gear_transcript *t = SDL_calloc(1, sizeof *t);
    SDL_strlcpy(t->path, path, sizeof t->path);
    t->keep = keep > 0 ? keep : 1;
    t->sent = SDL_calloc((size_t)t->keep, sizeof *t->sent);
    size_t n = 0;
    char *all = SDL_LoadFile(path, &n);
    for (char *line = all, *end; line && line < all + n; line = end + 1) {
        end = SDL_strchr(line, '\n');
        if (!end) end = all + n;
        gs_json *doc = gs_json_parse(line, (size_t)(end - line));
        if (!doc) continue;  // (a line cut short, by a crash mid-write)
        gs_jv o = gs_json_root(doc);
        const char *text = gs_json_str(gs_json_get(o, "text"), NULL);
        if (text) push(t, text, gs_json_str(gs_json_get(o, "at"), ""), gs_json_str(gs_json_get(o, "to"), ""));
        gs_json_free(doc);
    }
    SDL_free(all);
    return t;
}

void gear_transcript_close(gear_transcript *t) {
    if (!t) return;
    for (int i = 0; i < t->count; i++) SDL_free(t->sent[i].text);
    SDL_free(t->sent);
    SDL_free(t);
}

void gear_transcript_add(gear_transcript *t, const char *text, const char *to) {
    SDL_Time now;
    SDL_DateTime d;
    char at[24] = "";
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &d, true))
        SDL_snprintf(at, sizeof at, "%04d-%02d-%02d %02d:%02d", d.year, d.month, d.day, d.hour, d.minute);
    push(t, text, at, to ? to : "");
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "at", gs_jval_str(at));
    gs_jval_set(o, "to", gs_jval_str(to ? to : ""));
    gs_jval_set(o, "text", gs_jval_str(text));
    char *line = gs_jval_write(o, false);
    gs_jval_free(o);
    // A line cut short (a crash mid-write) is ended first, so it does not take this one with it.
    char last = '\n';
    SDL_IOStream *in = SDL_IOFromFile(t->path, "rb");
    if (in && SDL_SeekIO(in, -1, SDL_IO_SEEK_END) >= 0) SDL_ReadIO(in, &last, 1);
    if (in) SDL_CloseIO(in);
    SDL_IOStream *io = line ? SDL_IOFromFile(t->path, "ab") : NULL;
    if (io) {
        if (last != '\n') SDL_WriteIO(io, "\n", 1);
        SDL_WriteIO(io, line, SDL_strlen(line));
        SDL_WriteIO(io, "\n", 1);
        SDL_CloseIO(io);
    }
    SDL_free(line);
}

int gear_transcript_count(const gear_transcript *t) { return t ? t->count : 0; }

const gear_sent *gear_transcript_at(const gear_transcript *t, int i) { return &t->sent[i]; }
