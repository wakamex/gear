// Messages to one agent and its replies back, on a thread of its own: each message is sent through
// Wakterm, its reply waited for, and the reply handed back in order. A message sent while the agent
// is still on the last one waits its turn.
#pragma once
#include <stdbool.h>
#include "agent.h"

typedef struct gear_relay gear_relay;

gear_relay *gear_relay_new(const gear_agent *agent);
void gear_relay_free(gear_relay *r);  // gives up waiting for replies
void gear_relay_send(gear_relay *r, const char *text);
// The next reply, oldest first, or NULL when none has come (SDL_free it). A message that failed
// comes back as NULL with its reason in `error` and *failed set.
char *gear_relay_take(gear_relay *r, bool *failed, char *error, size_t error_size);
bool gear_relay_waiting(gear_relay *r);  // messages sent or queued whose replies have not come
