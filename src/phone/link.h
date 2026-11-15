// The phone's connection to gear on the computer: SSH to it with the phone's key, `gear serve` run there
// (which an authorized_keys line can force), and the frames of src/serve.h both ways, on a thread of
// its own, connecting again five seconds after a connection breaks. The server's host key is pinned:
// the first one seen is learned, and a different one later stops the link (GP_FAILED), as does the
// phone's key refused.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gp_link gp_link;

typedef struct {
    char host[256];
    int port;
    char user[64];
    const char *key_pem;     // the phone's private key
    const char *key_public;  // its OpenSSH public line
    char host_key[128];      // the server's key's SHA-256, base64, as pinned; "" to learn it
    long long seen;          // the number of the newest message the app has kept ('K'), 0 for none
} gp_link_config;

typedef enum { GP_CONNECTING, GP_OPEN, GP_FAILED } gp_link_state;

gp_link *gp_link_open(const gp_link_config *c);
void gp_link_close(gp_link *l);
gp_link_state gp_link_status(gp_link *l, char *why, size_t size);  // why it failed, or what it is doing
// Each connection starts by telling the computer the newest message's number ('K') the link has had,
// which the frames' own 'K' advance, so the computer gives again what came after it.
// The server's key as seen, once connected: to pin it when it was learned.
bool gp_link_host_key(gp_link *l, char *out, size_t size);
void gp_link_send(gp_link *l, char type, const void *data, uint32_t len);
// The next frame from the server, or false when none has come. SDL_free *data.
bool gp_link_take(gp_link *l, char *type, char **data, uint32_t *len);
// Whether the app is behind other apps, told to the computer now and on each connection ('B').
void gp_link_set_background(gp_link *l, bool behind);
// A function told of each frame as it comes, on the link's own thread, before gp_link_take has it:
// for a notification while the app's loop is paused in the background.
typedef void gp_link_heard(char type, const char *data, uint32_t len, void *user);
void gp_link_on_frame(gp_link *l, gp_link_heard *fn, void *user);
