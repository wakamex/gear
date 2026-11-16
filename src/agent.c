#include "agent.h"
#include <SDL3/SDL.h>
#include <string.h>
#include "gs_http.h"
#include "gs_json.h"
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static const char *wakterm(void) {
    const char *w = SDL_getenv("GEAR_WAKTERM");
    return w && *w ? w : "wakterm";
}

// GEAR_WAKTERM_HOST names a machine to run wakterm on over SSH, for agents on another computer;
// without it, the host gear_agent_set_host gave, if any.
static char fallback_host[256];

void gear_agent_set_host(const char *h) { SDL_strlcpy(fallback_host, h ? h : "", sizeof fallback_host); }

const char *gear_agent_host(void) {
    const char *h = SDL_getenv("GEAR_WAKTERM_HOST");
    return h && *h ? h : fallback_host[0] ? fallback_host : NULL;
}

static const char *host(void) { return gear_agent_host(); }

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
char *gear_host_run(const char *const *argv, const char *input, size_t *len, int *code) {
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
    char *out = gear_host_run(argv, input, &len, &code);
    if (!out) { SDL_snprintf(error, error_size, "Could not run %s: %s", argv[0], SDL_GetError()); return NULL; }
    gs_json *doc = code == 0 ? gs_json_parse(out, len) : NULL;
    if (!doc) SDL_snprintf(error, error_size, "%s %s %s failed (exit %d)", argv[0], argv[3], argv[4], code);
    SDL_free(out);
    return doc;
}

static void copy(char *dst, size_t size, gs_jv v) { SDL_strlcpy(dst, gs_json_str(v, ""), size); }

// ---- endchat ----

// endchat's profiles (https://github.com/wakamex/endchat), each a chat with an agent in ~/.endchat/NAME,
// served by `endchat serve NAME` on the profile's control.sock: listed while served, named for the
// profile, which is also its route's title in Panetone.

#ifndef _WIN32
// A connection to the profile's control socket, or -1 when nothing serves it.
static int endchat_connect(const char *profile) {
    struct sockaddr_un at = { .sun_family = AF_UNIX };
    if (SDL_snprintf(at.sun_path, sizeof at.sun_path, "%s/control.sock", profile) >= (int)sizeof at.sun_path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&at, sizeof at) != 0) close(fd), fd = -1;
    return fd;
}

// Each profile served now, as agents after `n` of `max` (named for the profile, in a "tab" of that name,
// which is also its route's in Panetone), busy while its state.json says a turn runs.
static int endchat_agents(gear_agent *out, int n, int max) {
    const char *home = SDL_getenv("HOME");
    char dir[1024];
    if (!home || host()) return n;  // (its profiles are on this machine)
    SDL_snprintf(dir, sizeof dir, "%s/.endchat", home);
    int count = 0;
    char **names = SDL_GlobDirectory(dir, "*", 0, &count);
    for (int i = 0; names && i < count && n < max; i++) {
        char profile[1200], state[1300];
        SDL_snprintf(profile, sizeof profile, "%s/%s", dir, names[i]);
        int fd = endchat_connect(profile);
        if (fd < 0) continue;
        close(fd);
        gear_agent *g = &out[n++];
        SDL_zerop(g);
        SDL_strlcpy(g->name, names[i], sizeof g->name);
        SDL_strlcpy(g->id, profile, sizeof g->id);
        SDL_strlcpy(g->harness, "endchat", sizeof g->harness);
        SDL_snprintf(state, sizeof state, "%s/state.json", profile);
        char *text = SDL_LoadFile(state, NULL);
        gs_json *doc = text ? gs_json_parse(text, SDL_strlen(text)) : NULL;
        SDL_strlcpy(g->status, doc && gs_json_bool(gs_json_get(gs_json_root(doc), "busy"), false) ? "busy" : "idle", sizeof g->status);
        gs_json_free(doc), SDL_free(text);
        SDL_strlcpy(g->tab, g->name, sizeof g->tab);
        g->pane = -1;
    }
    SDL_free(names);
    return n;
}

#else
static int endchat_agents(gear_agent *out, int n, int max) { (void)out, (void)max; return n; }
#endif

static bool is_endchat(const gear_agent *a) { return !SDL_strcmp(a->harness, "endchat"); }

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
    return endchat_agents(out, n, max);
}

void gear_agent_tabs(gear_agent *agents, int n) {
    for (int i = 0; i < n; i++)
        if (!is_endchat(&agents[i])) agents[i].tab[0] = 0;  // (an endchat profile's is its name)
    const char *argv[] = { wakterm(), "cli", "--no-auto-start", "list", "--format", "json", NULL };
    char ignored[256];
    gs_json *panes = n > 0 ? run_json(argv, NULL, ignored, sizeof ignored) : NULL;
    for (gs_jv p = panes ? gs_json_first(gs_json_root(panes)) : NULL; p; p = gs_json_next(p)) {
        int pane = (int)gs_json_num(gs_json_get(p, "pane_id"), -2);
        const char *title = gs_json_str(gs_json_get(p, "effective_title"), "");
        if (!*title) title = gs_json_str(gs_json_get(p, "tab_title"), "");
        for (int i = 0; i < n; i++)
            if (agents[i].pane == pane) SDL_strlcpy(agents[i].tab, title, sizeof agents[i].tab);
    }
    gs_json_free(panes);
}

bool gear_agent_find(const char *name, gear_agent *out, char *error, size_t error_size) {
    enum { MAX = 512 };
    gear_agent *all = SDL_malloc(MAX * sizeof *all);
    int n = gear_agents_list(all, MAX, error, error_size);
    bool found = false;
    for (int i = 0; i < n && !found; i++)
        if (!SDL_strcasecmp(all[i].name, name)) { *out = all[i]; found = true; }
    // No agent by that name: the agent in the tab of that title, whichever harness it runs.
    if (!found && n > 0) {
        gear_agent_tabs(all, n);
        for (int i = 0; i < n && !found; i++)
            if (!SDL_strcasecmp(all[i].tab, name)) { *out = all[i]; found = true; }
    }
    if (n >= 0 && !found) SDL_snprintf(error, error_size, "No live agent called %s, and none in a tab of that title", name);
    SDL_free(all);
    return found;
}

void gear_agent_route(const gear_agent *a, char *out, size_t size) {
    SDL_strlcpy(out, a->name, size);
    if (a->tab[0]) { SDL_strlcpy(out, a->tab, size); return; }
    gear_agent all[256];
    char error[256];
    int n = gear_agents_list(all, 256, error, sizeof error);
    if (n <= 0) return;
    gear_agent_tabs(all, n);
    for (int i = 0; i < n; i++)
        if (!SDL_strcmp(all[i].name, a->name) && all[i].tab[0]) SDL_strlcpy(out, all[i].tab, size);
}
