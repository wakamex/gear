// Coding agents reached through Wakterm's Agent API, by running the wakterm command (GEAR_WAKTERM
// names another): its catalog for the agents there are, `agent admit --return-final` to send a
// message as a prompt, and `agent request get` for the agent's final reply to it. Wakterm knows each
// harness (Claude Code, Codex, ...), so gear has no logic for any one agent. Each call starts a
// process and waits for it: call these from a worker thread or a command-line mode.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char name[128], id[128], incarnation[160], harness[32], status[32];
} gear_agent;

// The live agents, up to `max`; their count, or -1 with the reason in `error`.
int gear_agents_list(gear_agent *out, int max, char *error, size_t error_size);
// The live agent called `name`. False with the reason in `error`.
bool gear_agent_find(const char *name, gear_agent *out, char *error, size_t error_size);
// Sends `text` to the agent as a prompt and asks for its final reply, under a new request id
// written to `request_id`. False with the reason in `error`.
bool gear_agent_send(const gear_agent *a, const char *text, char request_id[37], char *error, size_t error_size);

typedef enum { GEAR_REPLY_PENDING, GEAR_REPLY_DONE, GEAR_REPLY_FAILED } gear_reply_state;
// Where the request stands. DONE sets *reply to the agent's final message (SDL_free it); FAILED
// puts the reason in `error`.
gear_reply_state gear_agent_reply(const char *request_id, char **reply, char *error, size_t error_size);
