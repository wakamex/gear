// The user's agents through Panetone (https://github.com/wakamex/panetone), on this machine or on the
// agents' machine over SSH (gear_agent_host), for the window and the phone's hub alike.
//
// A message goes as the user, `panetone say --to ROUTE --as VIA`: Panetone delivers it to the route's
// agent as if typed in its Telegram topic (queued for an agent in the middle of a turn, to an endchat
// profile alike) and posts a copy there, "[via VIA] TEXT".
//
// The answers come through `panetone watch`, every post Panetone makes to the user's channels, which
// gear_feed follows from where it last stopped. Panetone follows each turn of an agent with a
// "turn_end" line naming the message that answers what began the turn: of the posts of the route
// being talked to, that one is the reply, and the others are the agent's notes along the way.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Hands `text` to the agent of the route titled `route`, as the user through the client `via`
// ("gear", "gear-window"). False with the reason in `error`.
bool gear_panetone_say(const char *route, const char *text, const char *via, char *error, size_t error_size);

typedef enum {
    GEAR_POST_MESSAGE,  // a post to show: an agent's message, or Panetone's receipt or notice (quiet)
    GEAR_POST_MINE,     // the user's own message, sent through another client
    GEAR_POST_REPLY,    // the answer of the route being talked to, ending its turn
} gear_post_kind;

typedef struct {
    gear_post_kind kind;
    bool quiet;  // Panetone's receipt for a send, or its notice: shown quietly, never spoken
    bool shown;  // a reply given already as a message, its turn's end having come late: to speak only
    char who[128], at[8];  // the route's title, and the time HH:MM
    char *text;  // SDL_free it
} gear_post;

typedef struct gear_feed gear_feed;

// Follows the feed on a thread of its own, from the position kept in the file `seq_path` (from now
// on the first time; with "", from now on, nothing kept), leaving out the copies of the messages this
// client sent through `via`.
gear_feed *gear_feed_open(const char *seq_path, const char *via);
void gear_feed_close(gear_feed *f);
// The next post, with `route` the route being talked to now (its posts are held up to 3 s for the
// turn's end); false when there is none yet.
bool gear_feed_next(gear_feed *f, const char *route, gear_post *out);
