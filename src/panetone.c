#include "panetone.h"
#include <SDL3/SDL.h>
#include <string.h>
#include "gs_json.h"

static const char *panetone(void) {
    const char *p = SDL_getenv("GEAR_PANETONE");
    return p && *p ? p : "panetone";
}

bool gear_panetone_say(const char *route, const char *text, char *error, size_t error_size) {
    // (the text on standard input, so it is not in the process list)
    const char *argv[] = { panetone(), "say", "--to", route, "--as", "gear", NULL };
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) { SDL_snprintf(error, error_size, "could not run panetone: %s", SDL_GetError()); return false; }
    SDL_IOStream *in = SDL_GetProcessInput(p);
    SDL_WriteIO(in, text, SDL_strlen(text));
    SDL_CloseIO(in);
    size_t len = 0;
    int code = -1;
    char *out = SDL_ReadProcess(p, &len, &code);
    SDL_DestroyProcess(p);
    // {"ok": true, "result": {"request_id", "accepted", "already_sent"}}, or the reason it was not
    gs_json *doc = out ? gs_json_parse(out, strcspn(out, "\n")) : NULL;  // (its first line; a refusal's reason follows)
    gs_jv r = doc ? gs_json_root(doc) : NULL;
    bool ok = code == 0 && r && gs_json_bool(gs_json_get(r, "ok"), false) && gs_json_bool(gs_json_get(gs_json_get(r, "result"), "accepted"), false);
    if (!ok) {
        const char *why = r ? gs_json_str(gs_json_get(r, "error"), NULL) : NULL;
        if (!why && r) why = gs_json_str(gs_json_get(gs_json_get(r, "error"), "message"), NULL);
        size_t n = out ? strcspn(out, "\n") : 0;
        if (why) SDL_snprintf(error, error_size, "Panetone did not take the message for %s: %s", route, why);
        else SDL_snprintf(error, error_size, "Panetone did not take the message for %s: %.*s", route, (int)SDL_min(n, 300), out ? out : "no answer");
    }
    gs_json_free(doc);
    SDL_free(out);
    return ok;
}
