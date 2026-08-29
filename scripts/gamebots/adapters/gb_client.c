/* gb_client — see gb_client.h. Plain C89-ish, no dependencies beyond libc, so
 * it drops into a 1999 game engine's build without argument. */

#include "gb_client.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static uint64_t gb_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static void gb_fail(gb_client_t *c, const char *what)
{
    snprintf(c->last_error, sizeof(c->last_error), "%s: %s", what,
             strerror(errno));
    c->last_error_us = gb_now_us();
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

void gb_client_init(gb_client_t *c, const char *socket_path)
{
    const char *p;
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->timeout_us = GB_DEFAULT_TIMEOUT_US;

    p = socket_path;
    if (!p || !*p)
        p = getenv("GAMEBOTS_SOCKET");
    if (!p || !*p)
        p = "/run/user/1000/gamebots/policy.sock";

    /* sockaddr_un.sun_path is a fixed 108-byte array in the kernel ABI. Copying
     * past it truncates silently and connect() then fails on a path nobody
     * typed, so refuse loudly-ish here instead. */
    if (strlen(p) >= sizeof(c->socket_path)) {
        snprintf(c->last_error, sizeof(c->last_error),
                 "socket path too long (%zu >= %zu)", strlen(p),
                 sizeof(c->socket_path));
        c->socket_path[0] = '\0';
        return;
    }
    strcpy(c->socket_path, p);
}

void gb_client_close(gb_client_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static int gb_connect(gb_client_t *c)
{
    struct sockaddr_un addr;
    uint64_t now = gb_now_us();

    if (c->fd >= 0)
        return 0;
    if (!c->socket_path[0])
        return -1;
    /* A dead policy server must cost one failed connect every couple of
     * seconds, not one per bot per frame. */
    if (c->last_connect_attempt_us &&
        now - c->last_connect_attempt_us < GB_RECONNECT_COOLDOWN_US)
        return -1;
    c->last_connect_attempt_us = now;

    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->fd < 0) {
        gb_fail(c, "socket");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* gb_client_init already refused anything that would not fit, so this is
     * an exact copy of a known-short string. strncpy here trips
     * -Wstringop-truncation because the two buffers are the same size, and a
     * truncated socket path fails on an address nobody typed. */
    {
        size_t plen = strlen(c->socket_path);
        if (plen >= sizeof(addr.sun_path)) {
            gb_client_close(c);
            return -1;
        }
        memcpy(addr.sun_path, c->socket_path, plen + 1);
    }
    if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        gb_fail(c, "connect");
        return -1;
    }
    c->reconnects++;
    return 0;
}

void gb_begin(gb_client_t *c, uint32_t tick)
{
    gb_header_t *h = (gb_header_t *)c->req;
    c->tick = tick;
    c->n = 0;
    c->n_actions = 0;
    memcpy(h->magic, GB_REQ_MAGIC, 4);
    h->schema_hash = GB_SCHEMA_HASH;
    h->flags = GB_FLAG_NONE;
    h->tick = tick;
}

int gb_add(gb_client_t *c, uint16_t bot_id, const float *obs)
{
    gb_obs_entry_t *e;
    if (c->n >= GB_MAX_BOTS)
        return -1;
    e = (gb_obs_entry_t *)(c->req + sizeof(gb_header_t)) + c->n;
    e->bot_id = bot_id;
    e->pad = 0;
    /* One memcpy per bot. This is the whole reason the adapter is C: the
     * equivalent in the Python harness costs ~2.8us/bot, which at 512 bots is
     * more than the GPU forward pass it feeds. */
    memcpy(e->obs, obs, sizeof(float) * GB_OBS_DIM);
    c->n++;
    return 0;
}

static int gb_write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    while (len) {
        ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static int gb_read_all(gb_client_t *c, void *buf, size_t len, uint64_t deadline)
{
    unsigned char *p = (unsigned char *)buf;
    while (len) {
        struct timeval tv;
        fd_set rd;
        uint64_t now = gb_now_us();
        ssize_t r;

        if (now >= deadline)
            return -1;                       /* out of frame budget */
        tv.tv_sec = 0;
        tv.tv_usec = (suseconds_t)(deadline - now);
        FD_ZERO(&rd);
        FD_SET(c->fd, &rd);
        if (select(c->fd + 1, &rd, NULL, NULL, &tv) <= 0)
            return -1;
        r = recv(c->fd, p, len, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

gb_result_t gb_exchange(gb_client_t *c)
{
    gb_header_t *rh = (gb_header_t *)c->req;
    const gb_header_t *sh = (const gb_header_t *)c->resp;
    size_t req_len, resp_len;
    uint64_t deadline;

    if (c->n == 0) {
        c->frames_ok++;
        return GB_OK;                        /* nothing to ask about */
    }
    if (gb_connect(c) < 0) {
        c->frames_fallback++;
        return GB_FALLBACK;
    }

    rh->n_bots = c->n;
    req_len = sizeof(gb_header_t) + (size_t)c->n * sizeof(gb_obs_entry_t);
    if (gb_write_all(c->fd, c->req, req_len) < 0) {
        gb_fail(c, "send");
        c->frames_fallback++;
        return GB_FALLBACK;
    }

    deadline = gb_now_us() + (uint64_t)c->timeout_us;
    resp_len = sizeof(gb_header_t) + (size_t)c->n * sizeof(gb_action_t);
    if (gb_read_all(c, c->resp, resp_len, deadline) < 0) {
        /* Timed out or the peer went away. Drop the connection rather than
         * leaving a half-read response in the stream to desynchronise the
         * next frame — a desynchronised adapter feeds bot A's action to bot B,
         * which looks like a model bug and is not one. */
        gb_fail(c, "recv");
        c->frames_fallback++;
        return GB_FALLBACK;
    }

    if (memcmp(sh->magic, GB_RESP_MAGIC, 4) != 0 ||
        sh->schema_hash != GB_SCHEMA_HASH || sh->n_bots != c->n) {
        snprintf(c->last_error, sizeof(c->last_error),
                 "bad response (hash %08x vs %08x, %u vs %u bots)",
                 sh->schema_hash, (unsigned)GB_SCHEMA_HASH,
                 (unsigned)sh->n_bots, (unsigned)c->n);
        c->last_error_us = gb_now_us();
        gb_client_close(c);
        c->frames_fallback++;
        return GB_FALLBACK;
    }

    c->n_actions = sh->n_bots;
    c->frames_ok++;
    return GB_OK;
}

const gb_action_t *gb_action(const gb_client_t *c, uint16_t i)
{
    if (i >= c->n_actions)
        return NULL;
    return (const gb_action_t *)(c->resp + sizeof(gb_header_t)) + i;
}

static float gb_fin(float v, float lo, float hi)
{
    /* NaN fails every comparison, so it must be tested for explicitly — a
     * plain min/max chain passes it straight through. */
    if (v != v)
        return 0.0f;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void gb_clamp(gb_action_t *a)
{
    a->pitch_delta = gb_fin(a->pitch_delta, -GB_MAX_PITCH_DELTA_DEG,
                            GB_MAX_PITCH_DELTA_DEG);
    a->yaw_delta = gb_fin(a->yaw_delta, -GB_MAX_YAW_DELTA_DEG,
                          GB_MAX_YAW_DELTA_DEG);
    a->forward = gb_fin(a->forward, -1.0f, 1.0f);
    a->side = gb_fin(a->side, -1.0f, 1.0f);
}
