/* gb_client — the shared client every engine adapter links against.
 *
 * One implementation of the wire protocol, the batching, the timeout and the
 * fallback decision, so that adding an engine means writing observation
 * extraction and action application and NOTHING else. Four copies of this
 * logic across four engines would drift, and the drift would show up as one
 * game's bots behaving subtly differently for reasons nobody could find.
 *
 * The contract with the game server is the important part:
 *
 *   - It NEVER blocks longer than gb_client_t.timeout_us. A game server that
 *     stalls waiting for a bot brain is worse than one with stupid bots, so a
 *     late policy server is treated as no policy server.
 *   - On any failure it returns GB_FALLBACK and the caller runs the engine's
 *     own bot AI for that frame. Failure is expected, not exceptional: the
 *     policy server gets restarted whenever a model is promoted.
 *   - It reconnects lazily and never faster than GB_RECONNECT_COOLDOWN_US, so
 *     a dead policy server costs one failed connect() every few seconds
 *     instead of one per bot per frame.
 *
 * Usage from an engine adapter:
 *
 *     gb_client_t gb;
 *     gb_client_init(&gb, NULL);                  // NULL = default socket
 *     ...
 *     gb_begin(&gb, level.time);                  // once per server frame
 *     for (each bot) gb_add(&gb, bot_id, obs);    // obs is GB_OBS_DIM floats
 *     if (gb_exchange(&gb) == GB_OK)
 *         for (each bot) gb_action(&gb, i, &act);
 *     else
 *         run_builtin_bot_ai();
 */
#ifndef GB_CLIENT_H
#define GB_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include "../gamebots_schema.h"

#define GB_HEADER_MAX          16
#define GB_MAX_BOTS            256
#define GB_DEFAULT_TIMEOUT_US  4000     /* well inside a 10ms GoldSrc tick */
#define GB_RECONNECT_COOLDOWN_US 2000000

typedef enum {
    GB_OK = 0,
    GB_FALLBACK = 1        /* use the engine's own bot AI this frame */
} gb_result_t;

typedef struct {
    int      fd;                    /* -1 when disconnected */
    char     socket_path[108];      /* sun_path is 108 bytes; see gb_client.c */
    int      timeout_us;

    uint32_t tick;
    uint16_t n;
    unsigned char  req[GB_HEADER_MAX + GB_MAX_BOTS * sizeof(gb_obs_entry_t)];
    unsigned char  resp[GB_HEADER_MAX + GB_MAX_BOTS * sizeof(gb_action_t)];
    uint16_t n_actions;

    /* Observability. A silently-degraded adapter is the failure mode to fear:
     * the bots just quietly become the engine's own again and nobody notices
     * the expensive GPU is doing nothing. */
    uint64_t frames_ok;
    uint64_t frames_fallback;
    uint64_t reconnects;
    uint64_t last_connect_attempt_us;
    uint64_t last_error_us;
    char     last_error[128];
} gb_client_t;


void        gb_client_init(gb_client_t *c, const char *socket_path);
void        gb_client_close(gb_client_t *c);

/* Start a frame. Discards anything not exchanged from the previous one. */
void        gb_begin(gb_client_t *c, uint32_t tick);

/* Append one bot's observation. obs must be GB_OBS_DIM floats.
 * Returns 0 on success, -1 if the batch is full. */
int         gb_add(gb_client_t *c, uint16_t bot_id, const float *obs);

/* Send the batch and wait for actions, bounded by timeout_us. */
gb_result_t gb_exchange(gb_client_t *c);

/* Read back action i (0..n-1) from the last successful exchange. */
const gb_action_t *gb_action(const gb_client_t *c, uint16_t i);

/* Clamp an action to what a body can do. Applied by the ADAPTER because the
 * policy is not trusted: a half-trained net emits NaN long before it emits
 * good play, and NaN reaching a view angle makes the engine do strange things. */
void        gb_clamp(gb_action_t *a);

#endif /* GB_CLIENT_H */
