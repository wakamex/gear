// Messages to the user's agents through Panetone (https://github.com/wakamex/panetone), as the user:
// `panetone say --to ROUTE --as gear`, which delivers the text to the route's agent as if typed in
// its Telegram topic (queued for a busy agent, to an endchat profile alike) and posts a copy there,
// "[via gear] TEXT". The agent's answers come through `panetone watch`, which gear's hub follows.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Hands `text` to the agent of the route titled `route`. False with the reason in `error`.
bool gear_panetone_say(const char *route, const char *text, char *error, size_t error_size);
