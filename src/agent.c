#include "agent.h"
#include <SDL3/SDL.h>
#include "gs_http.h"
#include "gs_json.h"

static const char *wakterm(void) {
    const char *w = SDL_getenv("GEAR_WAKTERM");
    return w && *w ? w : "wakterm";
}

// Runs the command with `input` on its standard input, if any, and returns what it wrote to its
// standard output (SDL_free it), or NULL when it could not start. *code is its exit code.
static char *run(const char *const *argv, const char *input, size_t *len, int *code) {
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, input ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *p = gs_http_spawn(props);
    SDL_DestroyProperties(props);
    if (!p) return NULL;
    if (input) {
        SDL_IOStream *in = SDL_GetProcessInput(p);
        SDL_WriteIO(in, input, SDL_strlen(input));
        SDL_CloseIO(in);
    }
    int exit = 0;
    char *out = SDL_ReadProcess(p, len, &exit);
    *code = gs_http_exit_code(p, exit);
    SDL_DestroyProcess(p);
    return out;
}

// The command's standard output parsed as JSON, or NULL with the reason in `error`.
static gs_json *run_json(const char *const *argv, const char *input, char *error, size_t error_size) {
    size_t len = 0;
    int code = 0;
    char *out = run(argv, input, &len, &code);
    if (!out) { SDL_snprintf(error, error_size, "Could not run %s: %s", argv[0], SDL_GetError()); return NULL; }
    gs_json *doc = code == 0 ? gs_json_parse(out, len) : NULL;
    if (!doc) SDL_snprintf(error, error_size, "%s %s %s failed (exit %d)", argv[0], argv[2], argv[3], code);
    SDL_free(out);
    return doc;
}

static void copy(char *dst, size_t size, gs_jv v) { SDL_strlcpy(dst, gs_json_str(v, ""), size); }

int gear_agents_list(gear_agent *out, int max, char *error, size_t error_size) {
    const char *argv[] = { wakterm(), "cli", "agent", "catalog", NULL };
    gs_json *doc = run_json(argv, NULL, error, error_size);
    if (!doc) return -1;
    int n = 0;
    for (gs_jv a = gs_json_first(gs_json_get(gs_json_root(doc), "agents")); a && n < max; a = gs_json_next(a)) {
        if (!gs_json_bool(gs_json_get(a, "alive"), false)) continue;
        gear_agent *g = &out[n++];
        copy(g->name, sizeof g->name, gs_json_get(a, "name"));
        copy(g->id, sizeof g->id, gs_json_get(a, "agent_id"));
        copy(g->incarnation, sizeof g->incarnation, gs_json_get(a, "incarnation_id"));
        copy(g->harness, sizeof g->harness, gs_json_get(a, "harness"));
        copy(g->status, sizeof g->status, gs_json_get(a, "status"));
    }
    gs_json_free(doc);
    return n;
}

bool gear_agent_find(const char *name, gear_agent *out, char *error, size_t error_size) {
    enum { MAX = 512 };
    gear_agent *all = SDL_malloc(MAX * sizeof *all);
    int n = gear_agents_list(all, MAX, error, error_size);
    bool found = false;
    for (int i = 0; i < n && !found; i++)
        if (!SDL_strcasecmp(all[i].name, name)) { *out = all[i]; found = true; }
    if (n >= 0 && !found) SDL_snprintf(error, error_size, "No live agent called %s", name);
    SDL_free(all);
    return found;
}

// A random (version 4) UUID, for a request id.
static void uuid(char out[37]) {
    Uint8 b[16];
    for (int i = 0; i < 16; i++) b[i] = (Uint8)SDL_rand_bits();
    b[6] = (Uint8)((b[6] & 0x0f) | 0x40);
    b[8] = (Uint8)((b[8] & 0x3f) | 0x80);
    SDL_snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

bool gear_agent_send(const gear_agent *a, const char *text, char request_id[37], char *error, size_t error_size) {
    static bool seeded;
    if (!seeded) { SDL_srand(0); seeded = true; }
    uuid(request_id);
    // The text goes on standard input, never the command line, so nothing said shows in the process list.
    const char *argv[] = { wakterm(), "cli", "agent", "admit", "--exact-agent-id", "--incarnation", a->incarnation, "--request-id", request_id, "--return-final", a->id, NULL };
    gs_json *doc = run_json(argv, text, error, error_size);
    if (!doc) return false;
    // A refusal still exits 0: the answer says whether the prompt was written, and why not.
    gs_jv r = gs_json_root(doc);
    bool written = gs_json_bool(gs_json_get(r, "prompt_written"), false);
    if (!written) SDL_snprintf(error, error_size, "%s did not take the message: %s", a->name, gs_json_str(gs_json_get(r, "detail"), gs_json_str(gs_json_get(r, "status"), "no reason given")));
    gs_json_free(doc);
    return written;
}

gear_reply_state gear_agent_reply(const char *request_id, char **reply, char *error, size_t error_size) {
    *reply = NULL;
    const char *argv[] = { wakterm(), "cli", "agent", "request", "get", request_id, NULL };
    gs_json *doc = run_json(argv, NULL, error, error_size);
    if (!doc) return GEAR_REPLY_FAILED;
    gs_jv r = gs_json_root(doc);
    const char *state = gs_json_str(gs_json_get(r, "state"), "");
    gear_reply_state s = GEAR_REPLY_PENDING;
    if (!SDL_strcmp(state, "completed")) {
        *reply = SDL_strdup(gs_json_str(gs_json_get(r, "final_message"), ""));
        s = GEAR_REPLY_DONE;
    } else if (!SDL_strcmp(state, "failed") || !SDL_strcmp(state, "cancelled") || !SDL_strcmp(state, "timed_out") || !SDL_strcmp(state, "expired")) {
        const char *detail = gs_json_str(gs_json_get(r, "detail"), "");
        SDL_snprintf(error, error_size, "The agent's reply %s%s%s", state, *detail ? ": " : "", detail);
        s = GEAR_REPLY_FAILED;
    }
    gs_json_free(doc);
    return s;
}
