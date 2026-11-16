// The user's coding agents: Wakterm's, from its catalog (`wakterm cli agent catalog`, GEAR_WAKTERM
// naming another command), and endchat's profiles (below), on this machine or on another through ssh
// (GEAR_WAKTERM_HOST). Messages to them go through Panetone (panetone.h). Each call starts a process
// and waits for it: call these from a worker thread or a command-line mode.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char name[128], id[128], incarnation[160], harness[32], status[32];
    char tab[128];  // the title of its tab as Wakterm shows it, once gear_agent_tabs has filled it
    int pane;
} gear_agent;

// The live agents, up to `max`; their count, or -1 with the reason in `error`.
int gear_agents_list(gear_agent *out, int max, char *error, size_t error_size);
// Fills in the title each agent's tab shows (Wakterm's effective title: the one set, else the one it
// makes). Left empty where it cannot be read.
void gear_agent_tabs(gear_agent *agents, int n);
// The live agent called `name`, or else the one running in a tab titled `name`. False with the
// reason in `error`.
bool gear_agent_find(const char *name, gear_agent *out, char *error, size_t error_size);
// The title of Panetone's route for the agent, which is its tab's (an endchat profile's name), or its
// name when its tab has none: where gear sends to it and what names its conversation.
void gear_agent_route(const gear_agent *a, char *out, size_t size);

// Runs the command here, or on the agents' machine (gear_agent_host) through ssh, with `input` on
// its standard input here; what it wrote to its standard output (SDL_free it), or NULL when it could
// not start. *code is its exit code.
char *gear_host_run(const char *const *argv, const char *input, size_t *len, int *code);

// The machine wakterm runs on over SSH: GEAR_WAKTERM_HOST, else the one set here (a remembered
// target's), else NULL for this machine.
void gear_agent_set_host(const char *host);
const char *gear_agent_host(void);
