/* True-source tests for the shared engine-adapter client.
 *
 * Includes scripts/gamebots/adapters/gb_client.c directly rather than a copy,
 * so these test the code that actually links into a game server. The parts
 * worth pinning are the ones whose failure is silent inside an engine:
 *
 *   - a NaN reaching a view angle (a half-trained net emits NaN long before
 *     it emits good play, and NaN fails every comparison, so a plain min/max
 *     chain passes it straight through);
 *   - the request buffer being large enough for a full batch, since an
 *     overflow here corrupts the game server's own memory;
 *   - the batch cap being enforced rather than wrapping;
 *   - an over-long socket path being refused instead of silently truncated to
 *     an address nobody typed.
 *
 * Built and run by tests/run_all.sh section [2].
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../../scripts/gamebots/adapters/gb_client.c"

static int failures;
static int checks;

static void ok(const char *name, int cond)
{
    checks++;
    if (cond) {
        printf("  [ ok ] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
        failures++;
    }
}

static void test_clamp(void)
{
    gb_action_t a;

    memset(&a, 0, sizeof(a));
    a.pitch_delta = 1e9f;
    a.yaw_delta = -1e9f;
    a.forward = 40.0f;
    a.side = -40.0f;
    gb_clamp(&a);
    ok("huge values clamp to the bounds",
       a.pitch_delta == GB_MAX_PITCH_DELTA_DEG &&
       a.yaw_delta == -GB_MAX_YAW_DELTA_DEG &&
       a.forward == 1.0f && a.side == -1.0f);

    /* The one that matters. NaN < lo and NaN > hi are BOTH false, so a
     * min/max chain returns NaN unchanged and the engine gets a NaN view
     * angle. */
    memset(&a, 0, sizeof(a));
    a.pitch_delta = (float)NAN;
    a.yaw_delta = (float)NAN;
    a.forward = (float)NAN;
    a.side = (float)NAN;
    gb_clamp(&a);
    ok("NaN is replaced, not passed through",
       a.pitch_delta == 0.0f && a.yaw_delta == 0.0f &&
       a.forward == 0.0f && a.side == 0.0f);

    memset(&a, 0, sizeof(a));
    a.pitch_delta = (float)INFINITY;
    a.side = (float)(-INFINITY);
    gb_clamp(&a);
    ok("infinities clamp to the bounds",
       a.pitch_delta == GB_MAX_PITCH_DELTA_DEG && a.side == -1.0f);

    memset(&a, 0, sizeof(a));
    a.pitch_delta = 3.0f;
    a.yaw_delta = -4.0f;
    a.forward = 0.5f;
    a.side = -0.25f;
    gb_clamp(&a);
    ok("in-range actions are left alone",
       a.pitch_delta == 3.0f && a.yaw_delta == -4.0f &&
       a.forward == 0.5f && a.side == -0.25f);
}

static void test_wire_sizes(void)
{
    /* These must match the Python side exactly; the generated header carries
     * static asserts for them too, but a runtime check names the number. */
    ok("header is 16 bytes", sizeof(gb_header_t) == 16);
    ok("obs entry is 4 + 4*OBS_DIM bytes",
       sizeof(gb_obs_entry_t) == 4u + 4u * GB_OBS_DIM);
    ok("action is 24 bytes", sizeof(gb_action_t) == 24);

    {
        gb_client_t c;
        size_t need = sizeof(gb_header_t) +
                      (size_t)GB_MAX_BOTS * sizeof(gb_obs_entry_t);
        /* An undersized request buffer would be a heap overflow INSIDE a game
         * server, which is the worst possible place for one. */
        ok("request buffer holds a full batch", sizeof(c.req) >= need);
        need = sizeof(gb_header_t) +
               (size_t)GB_MAX_BOTS * sizeof(gb_action_t);
        ok("response buffer holds a full batch", sizeof(c.resp) >= need);
    }
}

static void test_batch_building(void)
{
    gb_client_t c;
    gb_header_t *h;
    float obs[GB_OBS_DIM];
    unsigned i;
    int rc;

    for (i = 0; i < GB_OBS_DIM; i++)
        obs[i] = (float)i;

    gb_client_init(&c, "/tmp/gb-test-not-used.sock");
    gb_begin(&c, 4242);
    h = (gb_header_t *)c.req;
    ok("begin stamps the request magic", memcmp(h->magic, GB_REQ_MAGIC, 4) == 0);
    ok("begin stamps the schema hash", h->schema_hash == GB_SCHEMA_HASH);
    ok("begin stamps the tick", h->tick == 4242);
    ok("begin resets the batch", c.n == 0);

    for (i = 0; i < GB_MAX_BOTS; i++) {
        rc = gb_add(&c, (uint16_t)i, obs);
        if (rc != 0)
            break;
    }
    ok("a full batch is accepted", c.n == GB_MAX_BOTS);

    /* Refuse rather than wrap: silently dropping the 257th bot would make one
     * bot per frame freeze for reasons nothing reports. */
    rc = gb_add(&c, 9999, obs);
    ok("adding past the cap is refused", rc == -1 && c.n == GB_MAX_BOTS);

    {
        const gb_obs_entry_t *e =
            (const gb_obs_entry_t *)(c.req + sizeof(gb_header_t));
        ok("first bot id landed", e[0].bot_id == 0);
        ok("last bot id landed", e[GB_MAX_BOTS - 1].bot_id == GB_MAX_BOTS - 1);
        ok("observation copied verbatim",
           e[3].obs[0] == 0.0f && e[3].obs[GB_OBS_DIM - 1] ==
           (float)(GB_OBS_DIM - 1));
    }
    gb_client_close(&c);
}

static void test_socket_path_guard(void)
{
    gb_client_t c;
    char longpath[300];

    memset(longpath, 'x', sizeof(longpath) - 1);
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1] = '\0';

    gb_client_init(&c, longpath);
    /* sun_path is a fixed 108-byte kernel array. Truncating silently would
     * make connect() fail on an address nobody typed. */
    ok("an over-long socket path is refused", c.socket_path[0] == '\0');
    ok("and the reason is recorded", strstr(c.last_error, "too long") != NULL);

    gb_client_init(&c, "/run/user/1000/gamebots/policy.sock");
    ok("a normal socket path is accepted",
       strcmp(c.socket_path, "/run/user/1000/gamebots/policy.sock") == 0);
    gb_client_close(&c);
}

static void test_fallback_when_no_server(void)
{
    gb_client_t c;
    float obs[GB_OBS_DIM];
    unsigned i;

    for (i = 0; i < GB_OBS_DIM; i++)
        obs[i] = 0.0f;

    gb_client_init(&c, "/tmp/gb-definitely-no-server-here.sock");
    gb_begin(&c, 1);
    for (i = 0; i < 8; i++)
        gb_add(&c, (uint16_t)i, obs);
    ok("no policy server falls back", gb_exchange(&c) == GB_FALLBACK);
    ok("fallback is counted", c.frames_fallback == 1);

    /* Second attempt must not retry immediately: a dead policy server should
     * cost one failed connect every couple of seconds, not one per frame. */
    gb_begin(&c, 2);
    for (i = 0; i < 8; i++)
        gb_add(&c, (uint16_t)i, obs);
    ok("the reconnect cooldown holds", gb_exchange(&c) == GB_FALLBACK);
    ok("and it did not reconnect", c.reconnects == 0);

    /* An empty batch is not a failure — a server with no bots this frame. */
    gb_begin(&c, 3);
    ok("an empty batch is OK, not a fallback", gb_exchange(&c) == GB_OK);
    gb_client_close(&c);
}

int main(void)
{
    printf("== gamebots engine-adapter client (true-source) ==\n");
    test_clamp();
    test_wire_sizes();
    test_batch_building();
    test_socket_path_guard();
    test_fallback_when_no_server();
    printf("-- gamebots client: %d/%d checks passed --\n",
           checks - failures, checks);
    return failures ? 1 : 0;
}
