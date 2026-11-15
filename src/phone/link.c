#define _POSIX_C_SOURCE 200809L  // (getaddrinfo)
#include "link.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "key.h"
#include "libssh2.h"

typedef struct frame { struct frame *next; char type; uint32_t len; char *data; } frame;

struct gp_link {
    gp_link_config c;
    char *pem, *pub;
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_AtomicInt quit;
    gp_link_state state;
    char why[256], seen_key[128];
    // Bytes waiting to go out, and frames come in.
    char *out;
    size_t out_len, out_cap;
    frame *in;
    SDL_AtomicInt background;  // the app is behind others, told to the computer on each connection
    gp_link_heard *heard;      // told of each frame as it comes, on this thread
    void *heard_user;
};

static void set_state(gp_link *l, gp_link_state state, const char *why) {
    SDL_LockMutex(l->lock);
    l->state = state;
    SDL_strlcpy(l->why, why, sizeof l->why);
    SDL_UnlockMutex(l->lock);
}

static int dial(const char *host, int port, char *why, size_t size) {
    char service[16];
    SDL_snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *found = NULL;
    int err = getaddrinfo(host, service, &hints, &found);
    if (err) { SDL_snprintf(why, size, "cannot find %s: %s", host, gai_strerror(err)); return -1; }
    int fd = -1;
    for (struct addrinfo *a = found; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        // Ten seconds to answer, then the next address.
        fcntl(fd, F_SETFL, O_NONBLOCK);
        if (connect(fd, a->ai_addr, a->ai_addrlen) < 0 && errno != EINPROGRESS) { close(fd), fd = -1; continue; }
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int soerr = 0;
        socklen_t sl = sizeof soerr;
        if (poll(&p, 1, 10000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr) { close(fd), fd = -1; continue; }
        fcntl(fd, F_SETFL, 0);
    }
    freeaddrinfo(found);
    if (fd < 0) SDL_snprintf(why, size, "cannot reach %s port %d", host, port);
    return fd;
}

static void ssh_error(LIBSSH2_SESSION *s, const char *what, char *why, size_t size) {
    char *msg = NULL;
    libssh2_session_last_error(s, &msg, NULL, 0);
    SDL_snprintf(why, size, "%s: %s", what, msg ? msg : "unknown error");
}

static int sign(LIBSSH2_SESSION *s, unsigned char **sig, size_t *sig_len, const unsigned char *data, size_t data_len, void **abstract) {
    unsigned char out[96];
    size_t n = gp_key_sign(*(char **)abstract, data, data_len, out, sizeof out);
    if (!n || !(*sig = malloc(n))) return -1;  // (libssh2 frees it with its allocator, malloc's by default)
    SDL_memcpy(*sig, out, n);
    *sig_len = n;
    return 0;
}

static bool quitting(gp_link *l) { return SDL_GetAtomicInt(&l->quit) != 0; }

static void take_frames(gp_link *l, char *buf, size_t *len) {
    for (;;) {
        if (*len < 5) return;
        uint32_t n = (uint32_t)(uint8_t)buf[1] | (uint32_t)(uint8_t)buf[2] << 8 | (uint32_t)(uint8_t)buf[3] << 16 | (uint32_t)(uint8_t)buf[4] << 24;
        if (*len < 5 + (size_t)n) return;
        frame *f = SDL_calloc(1, sizeof *f);
        f->type = buf[0], f->len = n, f->data = SDL_malloc(n + 1);
        SDL_memcpy(f->data, buf + 5, n);
        f->data[n] = 0;
        SDL_memmove(buf, buf + 5 + n, *len - 5 - n);
        *len -= 5 + n;
        if (l->heard) l->heard(f->type, f->data, f->len, l->heard_user);
        SDL_LockMutex(l->lock);
        frame **q = &l->in;
        while (*q) q = &(*q)->next;
        *q = f;
        SDL_UnlockMutex(l->lock);
    }
}

// One connection, until it breaks: false when it cannot work again as it is (the phone's key refused,
// the computer's changed), with why in `why`.
static bool session(gp_link *l, char *why, size_t why_size) {
    bool again = true;
    set_state(l, GP_CONNECTING, "connecting");
    int fd = dial(l->c.host, l->c.port, why, why_size);
    if (fd < 0) return true;
    LIBSSH2_SESSION *s = libssh2_session_init();
    LIBSSH2_CHANNEL *ch = NULL;
    libssh2_session_set_blocking(s, 1);
    libssh2_session_set_timeout(s, 15000);
    if (libssh2_session_handshake(s, fd)) { ssh_error(s, "SSH failed", why, why_size); goto failed; }
    // The server's key, pinned the first time.
    const unsigned char *hash = (const unsigned char *)libssh2_hostkey_hash(s, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash) { SDL_strlcpy(why, "the server's key could not be read", why_size); goto failed; }
    char seen[128];
    {
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t o = 0;
        for (int i = 0; i < 32; i += 3) {
            unsigned v = (unsigned)hash[i] << 16 | (i + 1 < 32 ? (unsigned)hash[i + 1] << 8 : 0) | (i + 2 < 32 ? hash[i + 2] : 0);
            seen[o++] = b64[v >> 18 & 63], seen[o++] = b64[v >> 12 & 63];
            if (i + 1 < 32) seen[o++] = b64[v >> 6 & 63];
            if (i + 2 < 32) seen[o++] = b64[v & 63];
        }
        seen[o] = 0;  // (as ssh-keygen -l prints it, without the padding)
    }
    SDL_LockMutex(l->lock);
    SDL_strlcpy(l->seen_key, seen, sizeof l->seen_key);
    SDL_UnlockMutex(l->lock);
    if (l->c.host_key[0] && SDL_strcmp(l->c.host_key, seen)) {
        SDL_snprintf(why, why_size, "the server's key has changed (SHA256:%s): not connecting", seen);
        again = false;
        goto failed;
    }
    set_state(l, GP_CONNECTING, "signing in");
    // The phone's own signature, made with mbedTLS here: libssh2's mbedTLS code reads a key from
    // memory only when the byte after it is zero, which it never writes.
    unsigned char blob[128];
    size_t blob_len = gp_key_blob(l->pem, blob, sizeof blob);
    if (!blob_len || libssh2_userauth_publickey(s, l->c.user, blob, blob_len, sign, (void **)&l->pem)) {
        ssh_error(s, "the server did not accept the phone's key", why, why_size);
        again = false;
        goto failed;
    }
    if (!(ch = libssh2_channel_open_session(s)) || libssh2_channel_exec(ch, "gear serve")) { ssh_error(s, "could not start gear there", why, why_size); goto failed; }
    // What was waiting for the connection before is dropped (audio of a moment gone), and the computer
    // learns at once whether the app is behind others, so it sends no speech to play late.
    SDL_LockMutex(l->lock);
    l->out_len = 0;
    SDL_UnlockMutex(l->lock);
    gp_link_send(l, 'B', SDL_GetAtomicInt(&l->background) ? "1" : "0", 1);
    set_state(l, GP_CONNECTING, "loading the models");
    libssh2_keepalive_config(s, 1, 20);
    libssh2_session_set_blocking(s, 0);
    char *in = SDL_malloc(1 << 20);
    size_t in_len = 0;
    bool open_seen = false;
    while (!quitting(l)) {
        // What is waiting goes out, as much as the channel takes.
        SDL_LockMutex(l->lock);
        ssize_t w = l->out_len ? libssh2_channel_write(ch, l->out, l->out_len) : 0;
        if (w > 0) SDL_memmove(l->out, l->out + w, l->out_len - (size_t)w), l->out_len -= (size_t)w;
        bool more = l->out_len > 0;
        SDL_UnlockMutex(l->lock);
        if (w < 0 && w != LIBSSH2_ERROR_EAGAIN) { ssh_error(s, "the connection broke", why, why_size); break; }
        ssize_t r = libssh2_channel_read(ch, in + in_len, (1 << 20) - in_len);
        if (r > 0) {
            in_len += (size_t)r;
            take_frames(l, in, &in_len);
            if (!open_seen) open_seen = true, set_state(l, GP_OPEN, "connected");
            continue;
        }
        if (r < 0 && r != LIBSSH2_ERROR_EAGAIN) { ssh_error(s, "the connection broke", why, why_size); break; }
        if (libssh2_channel_eof(ch)) {
            char err[200] = "";
            libssh2_channel_read_stderr(ch, err, sizeof err - 1);
            SDL_snprintf(why, why_size, "gear on the computer stopped%s%s", *err ? ": " : "", err);
            break;
        }
        int next;
        libssh2_keepalive_send(s, &next);
        if (!more) {
            struct pollfd p = { .fd = fd, .events = POLLIN };
            poll(&p, 1, 20);
        }
    }
    SDL_free(in);
failed:
    if (ch) libssh2_channel_free(ch);
    libssh2_session_disconnect(s, "bye");
    libssh2_session_free(s);
    close(fd);
    return again;
}

// Connections one after another, five seconds apart, for as long as the app keeps the link, on this
// thread: in the background too, where the app's own loop is paused.
static int run(void *user) {
    gp_link *l = user;
    char why[256];
    while (!quitting(l)) {
        bool again = session(l, why, sizeof why);
        if (quitting(l)) break;
        if (!again) { set_state(l, GP_FAILED, why); break; }
        char wait[300];
        SDL_snprintf(wait, sizeof wait, "%s; again in a moment", why);
        set_state(l, GP_CONNECTING, wait);
        for (int i = 0; i < 50 && !quitting(l); i++) SDL_Delay(100);
    }
    return 0;
}

void gp_link_set_background(gp_link *l, bool behind) {
    if (SDL_GetAtomicInt(&l->background) == behind) return;
    SDL_SetAtomicInt(&l->background, behind);
    gp_link_send(l, 'B', behind ? "1" : "0", 1);
}

void gp_link_on_frame(gp_link *l, gp_link_heard *fn, void *user) {
    SDL_LockMutex(l->lock);
    l->heard = fn, l->heard_user = user;
    SDL_UnlockMutex(l->lock);
}

gp_link *gp_link_open(const gp_link_config *c) {
    static bool started;
    if (!started) libssh2_init(0), started = true;
    gp_link *l = SDL_calloc(1, sizeof *l);
    l->c = *c;
    l->pem = SDL_strdup(c->key_pem), l->pub = SDL_strdup(c->key_public);
    l->c.key_pem = l->pem, l->c.key_public = l->pub;
    l->lock = SDL_CreateMutex();
    l->state = GP_CONNECTING;
    l->thread = SDL_CreateThread(run, "gear-link", l);
    return l;
}

void gp_link_close(gp_link *l) {
    if (!l) return;
    SDL_SetAtomicInt(&l->quit, 1);
    SDL_WaitThread(l->thread, NULL);
    for (frame *f = l->in, *next; f; f = next) next = f->next, SDL_free(f->data), SDL_free(f);
    SDL_free(l->out);
    SDL_DestroyMutex(l->lock);
    SDL_free(l->pem), SDL_free(l->pub);
    SDL_free(l);
}

gp_link_state gp_link_status(gp_link *l, char *why, size_t size) {
    SDL_LockMutex(l->lock);
    gp_link_state st = l->state;
    if (why) SDL_strlcpy(why, l->why, size);
    SDL_UnlockMutex(l->lock);
    return st;
}

bool gp_link_host_key(gp_link *l, char *out, size_t size) {
    SDL_LockMutex(l->lock);
    bool have = l->seen_key[0] != 0;
    SDL_strlcpy(out, l->seen_key, size);
    SDL_UnlockMutex(l->lock);
    return have;
}

void gp_link_send(gp_link *l, char type, const void *data, uint32_t len) {
    SDL_LockMutex(l->lock);
    size_t need = l->out_len + 5 + len;
    if (need > l->out_cap) {
        while (l->out_cap < need) l->out_cap = l->out_cap ? l->out_cap * 2 : 65536;
        l->out = SDL_realloc(l->out, l->out_cap);
    }
    char *at = l->out + l->out_len;
    at[0] = type, at[1] = (char)len, at[2] = (char)(len >> 8), at[3] = (char)(len >> 16), at[4] = (char)(len >> 24);
    if (len) SDL_memcpy(at + 5, data, len);
    l->out_len = need;
    SDL_UnlockMutex(l->lock);
}

bool gp_link_take(gp_link *l, char *type, char **data, uint32_t *len) {
    SDL_LockMutex(l->lock);
    frame *f = l->in;
    if (f) l->in = f->next;
    SDL_UnlockMutex(l->lock);
    if (!f) return false;
    *type = f->type, *data = f->data, *len = f->len;
    SDL_free(f);
    return true;
}
