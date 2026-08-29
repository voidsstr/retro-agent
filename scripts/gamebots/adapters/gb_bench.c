/* gb_bench — what an engine adapter actually pays, measured in C.
 *
 * The Python harness measures the harness. This measures the real thing: the
 * memcpy per bot, the socket round trip, and the fallback path, from the same
 * code an engine adapter will link.
 *
 *   gcc -O2 gb_bench.c gb_client.c -o gb_bench -lm && ./gb_bench
 */
#include "gb_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    static float obs[GB_MAX_BOTS][GB_OBS_DIM];
    static uint64_t lat[4000];
    gb_client_t gb;
    int sizes[] = {4, 16, 32, 64, 128, 256};
    int iters = (argc > 1) ? atoi(argv[1]) : 2000;
    unsigned s, i, b;

    for (b = 0; b < GB_MAX_BOTS; b++)
        for (i = 0; i < GB_OBS_DIM; i++)
            obs[b][i] = (float)((b + i) % 20) * 0.05f;

    gb_client_init(&gb, NULL);
    printf("socket: %s\n", gb.socket_path);
    printf("schema: 0x%08x  obs_dim=%d  req/bot=%zu B  resp/bot=%zu B\n\n",
           GB_SCHEMA_HASH, GB_OBS_DIM, sizeof(gb_obs_entry_t),
           sizeof(gb_action_t));
    printf("%6s %10s %9s %9s %9s %12s %10s\n",
           "bots", "mean us", "p50 us", "p99 us", "us/bot", "decisions/s",
           "% of 50ms");

    for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int n = sizes[s];
        uint64_t sum = 0;
        int ok = 0, fb = 0;
        if (iters > (int)(sizeof(lat) / sizeof(lat[0])))
            iters = sizeof(lat) / sizeof(lat[0]);

        for (i = 0; i < 50; i++) {           /* warm */
            gb_begin(&gb, i);
            for (b = 0; b < (unsigned)n; b++) gb_add(&gb, b, obs[b]);
            gb_exchange(&gb);
        }
        for (i = 0; i < (unsigned)iters; i++) {
            uint64_t t0, t1;
            gb_begin(&gb, i);
            for (b = 0; b < (unsigned)n; b++) gb_add(&gb, (uint16_t)b, obs[b]);
            t0 = now_us();
            if (gb_exchange(&gb) == GB_OK) ok++; else fb++;
            t1 = now_us();
            lat[i] = t1 - t0;
            sum += lat[i];
        }
        qsort(lat, iters, sizeof(lat[0]), cmp_u64);
        {
            double mean = (double)sum / iters;
            printf("%6d %10.1f %9llu %9llu %9.2f %12.0f %9.2f%%\n", n, mean,
                   (unsigned long long)lat[iters / 2],
                   (unsigned long long)lat[(int)(iters * 0.99)],
                   mean / n, n / (mean / 1e6), mean / 50000.0 * 100.0);
        }
        if (fb) printf("       (%d fallbacks: %s)\n", fb, gb.last_error);
    }

    printf("\nframes ok=%llu fallback=%llu reconnects=%llu\n",
           (unsigned long long)gb.frames_ok,
           (unsigned long long)gb.frames_fallback,
           (unsigned long long)gb.reconnects);

    /* The property that matters more than speed: a dead policy server must
     * degrade to the engine's own AI, promptly, without wedging the frame. */
    printf("\n-- fallback when the policy server is gone --\n");
    {
        gb_client_t dead;
        uint64_t t0, t1;
        gb_client_init(&dead, "/run/user/1000/gamebots/definitely-not-here.sock");
        gb_begin(&dead, 0);
        for (b = 0; b < 32; b++) gb_add(&dead, (uint16_t)b, obs[b]);
        t0 = now_us();
        gb_result_t r = gb_exchange(&dead);
        t1 = now_us();
        printf("  first attempt: %s in %llu us (%s)\n",
               r == GB_FALLBACK ? "FALLBACK" : "ok",
               (unsigned long long)(t1 - t0), dead.last_error);
        gb_begin(&dead, 1);
        for (b = 0; b < 32; b++) gb_add(&dead, (uint16_t)b, obs[b]);
        t0 = now_us();
        r = gb_exchange(&dead);
        t1 = now_us();
        printf("  second attempt (cooldown): %s in %llu us\n",
               r == GB_FALLBACK ? "FALLBACK" : "ok",
               (unsigned long long)(t1 - t0));
        gb_client_close(&dead);
    }
    gb_client_close(&gb);
    return 0;
}
