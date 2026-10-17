#include "agent.h"
#include <SDL3/SDL.h>
#include "gs_http.h"
#include "gs_json.h"

static const char *wakterm(void) {
    const char *w = SDL_getenv("GEAR_WAKTERM");
    return w && *w ? w : "wakterm";
}

// GEAR_WAKTERM_HOST names a machine to run wakterm on over SSH, for agents on another computer.
static const char *host(void) {
    const char *h = SDL_getenv("GEAR_WAKTERM_HOST");
    return h && *h ? h : NULL;
}

// An argument in single quotes for a POSIX shell, a quote inside it closed, escaped and reopened.
static void shell_quote(char *out, size_t size, const char *a) {
    size_t o = 0;
    out[o++] = '\'';
    for (const char *p = a; *p && o + 6 < size; p++) {
        if (*p == '\'') { SDL_memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *p;
    }
    out[o++] = '\'';
    out[o++] = ' ';
    out[o] = 0;
}

// Over SSH the session can stay open after the remote command has exited (an admitted prompt keeps
// it open from Windows), so the output is read as it comes and the ssh process ended once it holds
// a whole JSON object, which is all any of these commands print; 30 s without one ends it too.
static bool json_whole(const char *s, size_t n) {
    int depth = 0;
    bool string = false, escape = false, seen = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') string = false;
        } else if (c == '"') string = true;
        else if (c == '{' || c == '[') depth++, seen = true;
        else if (c == '}' || c == ']') depth--;
    }
    return seen && depth == 0;
}

static char *read_answer(SDL_Process *p, size_t *len, int *exit, bool *ended) {
    SDL_IOStream *o = SDL_GetProcessOutput(p);
    size_t n = 0, cap = 4096;
    char *buf = SDL_malloc(cap);
    Uint64 deadline = SDL_GetTicks() + 30000;
    for (;;) {
        if (n + 4096 > cap) buf = SDL_realloc(buf, cap *= 2);
        size_t got = SDL_ReadIO(o, buf + n, cap - n - 1);
        n += got;
        if (got == 0) {
            SDL_IOStatus st = SDL_GetIOStatus(o);
            if (st != SDL_IO_STATUS_NOT_READY && st != SDL_IO_STATUS_READY) break;  // the end, or an error
            if (SDL_GetTicks() > deadline) break;
            SDL_Delay(5);
            continue;
        }
        if (json_whole(buf, n)) break;
    }
    buf[n] = 0;
    *len = n;
    // Done whether or not the session has closed: whatever the remote side still holds is ended.
    *ended = !SDL_WaitProcess(p, false, exit);
    if (*ended) {
        SDL_KillProcess(p, true);
        SDL_WaitProcess(p, true, NULL);
        *exit = json_whole(buf, n) ? 0 : -1;
    }
    return buf;
}

// Runs the command with `input` on its standard input, if any, and returns what it wrote to its
// standard output (SDL_free it), or NULL when it could not start. *code is its exit code.
//
// With GEAR_WAKTERM_HOST, the command runs there through ssh as one shell line. Its output goes to a
// file that is then printed: wakterm's prompt admission leaves the SSH session's output open after it
// exits, and the ssh client would wait on it.
static char *run(const char *const *argv, const char *input, size_t *len, int *code) {
    const char *remote[5] = { "ssh", "-T", host(), NULL, NULL };  // -T: no terminal, which would stay open
    char *line = NULL;
    if (remote[2]) {
        size_t size = 256;
        for (int i = 0; argv[i]; i++) size += 4 * SDL_strlen(argv[i]) + 4;
        line = SDL_malloc(size);
        SDL_strlcpy(line, "f=$(mktemp) || exit 1; ", size);
        for (int i = 0; argv[i]; i++) shell_quote(line + SDL_strlen(line), size - SDL_strlen(line), argv[i]);
        SDL_strlcat(line, "</dev/null >\"$f\" 2>/dev/null; c=$?; cat \"$f\"; rm -f \"$f\"; exit $c", size);
        remote[3] = line;
        input = NULL;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)(remote[2] ? remote : argv));
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, input ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *p = gs_http_spawn(props);
    SDL_DestroyProperties(props);
    SDL_free(line);
    if (!p) return NULL;
    if (input) {
        SDL_IOStream *in = SDL_GetProcessInput(p);
        SDL_WriteIO(in, input, SDL_strlen(input));
        SDL_CloseIO(in);
    }
    int exit = 0;
    bool ended = false;
    char *out = remote[2] ? read_answer(p, len, &exit, &ended) : SDL_ReadProcess(p, len, &exit);
    *code = ended ? exit : gs_http_exit_code(p, exit);  // an ssh ended by gear has its answer, whatever its code
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
    if (!doc) SDL_snprintf(error, error_size, "%s %s %s failed (exit %d)", argv[0], argv[3], argv[4], code);
    SDL_free(out);
    return doc;
}

static void copy(char *dst, size_t size, gs_jv v) { SDL_strlcpy(dst, gs_json_str(v, ""), size); }

int gear_agents_list(gear_agent *out, int max, char *error, size_t error_size) {
    const char *argv[] = { wakterm(), "cli", "--no-auto-start", "agent", "catalog", NULL };
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
        g->pane = (int)gs_json_num(gs_json_get(a, "pane_id"), -1);
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
    // No agent by that name: the agent in the tab of that title, whichever harness it runs.
    const char *argv[] = { wakterm(), "cli", "--no-auto-start", "list", "--format", "json", NULL };
    char ignored[256];
    gs_json *panes = found || n <= 0 ? NULL : run_json(argv, NULL, ignored, sizeof ignored);
    for (gs_jv p = panes ? gs_json_first(gs_json_root(panes)) : NULL; p && !found; p = gs_json_next(p)) {
        if (SDL_strcasecmp(gs_json_str(gs_json_get(p, "tab_title"), ""), name)) continue;
        int pane = (int)gs_json_num(gs_json_get(p, "pane_id"), -2);
        for (int i = 0; i < n && !found; i++)
            if (all[i].pane == pane) { *out = all[i]; found = true; }
    }
    gs_json_free(panes);
    if (n >= 0 && !found) SDL_snprintf(error, error_size, "No live agent called %s, and none in a tab of that title", name);
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
    // Locally the text goes on standard input, so nothing said shows in the process list. Over SSH it
    // is the last argument: the Windows ssh client never passes on the end of piped input.
    bool ssh = host() != NULL;
    const char *argv[] = { wakterm(), "cli", "--no-auto-start", "agent", "admit", "--exact-agent-id", "--incarnation", a->incarnation, "--request-id", request_id, "--return-final", a->id, ssh ? text : NULL, NULL };
    gs_json *doc = run_json(argv, ssh ? NULL : text, error, error_size);
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
    const char *argv[] = { wakterm(), "cli", "--no-auto-start", "agent", "request", "get", request_id, NULL };
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
