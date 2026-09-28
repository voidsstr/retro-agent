/*
 * conring.h - the bounded, never-blocking queue between the agent's loggers
 * and its console.
 *
 * THE DEFECT (agent <= 1.89.1, agent/src/log.c raw_out)
 * -----------------------------------------------------
 * Every log line was echoed to the console with
 * WriteFile(GetStdHandle(STD_ERROR_HANDLE)) WHILE HOLDING g_log_cs. A console
 * write BLOCKS whenever the console is not being serviced: a QuickEdit/Mark
 * selection in the agent's window, a hung conhost/csrss, or a hung display
 * (on NT the console is drawn by a GUI process). The thread inside that write
 * keeps the log lock, so every other thread that logs - the accept loop, every
 * command handler, the flusher - queues behind it, and the agent stops
 * answering while the OS is fine: new TCP connections time out and are then
 * refused. Seen on ADMIN-PC (Win7, .195) 2026-09-26 23:46 -> 09-28 11:13 after
 * its Radeon hung (TDR 0x117 with no recovery): agent.log simply stops.
 *
 * THE FIX
 * -------
 * Loggers only ever COPY a line into this ring (under the log lock, a memcpy)
 * and a dedicated low-priority echo thread copies lines OUT (under the same
 * lock, a memcpy) and writes them to the console with NO lock held. If the
 * console stops accepting output only the echo thread stops; the ring fills
 * and further lines are DROPPED and COUNTED - never waited for. agent.log is
 * written by a separate path and gets every line regardless.
 *
 * Drop discipline ("latch"): once a line has been dropped, the ring refuses
 * every line until the echo thread has drained it completely. So there is
 * exactly ONE gap per stall, it sits exactly where the ring's contents end,
 * and the take that empties the ring reports how many lines fell into it -
 * which the echo thread prints as a notice at precisely that point. A line is
 * always queued whole or not at all; the console never shows a torn line.
 *
 * Wake discipline: a push reports CONRING_WAKE only when the ring was EMPTY,
 * i.e. when the consumer may be asleep. The consumer sleeps only after a take
 * that returned no bytes and no gap. Both sides run under the same lock, so a
 * push that lands after the consumer's empty take also sees the ring empty and
 * wakes it: no lost wake-up, and no event signalled per line.
 *
 * NOT thread-safe on its own: every call is made with the log lock held.
 * Header-only and free of Win32 so tests/native/test_conring.c compiles the
 * code the agent runs.
 */
#ifndef RETRO_CONRING_H
#define RETRO_CONRING_H

#include <string.h>

#ifdef __GNUC__
#define CONRING_UNUSED __attribute__((unused))
#else
#define CONRING_UNUSED
#endif

#define CONRING_QUEUED  1u   /* the line is in the ring                     */
#define CONRING_WAKE    2u   /* the ring was empty: signal the consumer     */

typedef struct {
    char         *buf;
    unsigned int  cap;          /* a power of two (0 = echo disabled)       */
    unsigned int  head;         /* bytes ever queued, modulo 2^32           */
    unsigned int  tail;         /* bytes ever taken, modulo 2^32            */
    int           latched;      /* a line was dropped since the last drain  */
    unsigned int  gap_lines;    /* lines dropped in the current stall       */
    unsigned long total_dropped;        /* lines dropped, lifetime          */
    unsigned long total_dropped_bytes;  /* bytes dropped, lifetime          */
} conring_t;

/* Use the largest power of two that fits in `size` bytes of `storage`. */
CONRING_UNUSED static void conring_init(conring_t *r, char *storage,
                                        unsigned int size)
{
    unsigned int cap = 0;
    if (storage && size >= 2) {
        cap = 1;
        while (cap <= size / 2)
            cap <<= 1;
    }
    r->buf = storage;
    r->cap = cap;
    r->head = 0;
    r->tail = 0;
    r->latched = 0;
    r->gap_lines = 0;
    r->total_dropped = 0;
    r->total_dropped_bytes = 0;
}

CONRING_UNUSED static unsigned int conring_used(const conring_t *r)
{
    return r->head - r->tail;           /* unsigned: correct across 2^32 */
}

/* Queue one whole line, or drop it. Never waits for anything. Returns a mix
 * of CONRING_QUEUED and CONRING_WAKE (0 = dropped, consumer busy). */
CONRING_UNUSED static unsigned int conring_push(conring_t *r, const char *s,
                                                unsigned int len)
{
    unsigned int was_empty = (r->head == r->tail) ? CONRING_WAKE : 0;
    unsigned int off, first;

    if (len == 0)
        return 0;
    if (r->latched || len > r->cap - conring_used(r)) {
        /* Full, or still draining an earlier stall: drop and count. A drop
         * onto an EMPTY ring (a line bigger than the whole ring) still wakes
         * the consumer, whose empty take then reports the gap and clears the
         * latch - otherwise nothing would ever clear it. */
        r->latched = 1;
        r->gap_lines++;
        r->total_dropped++;
        r->total_dropped_bytes += len;
        return was_empty;
    }
    off = r->head & (r->cap - 1);
    first = r->cap - off;
    if (first > len)
        first = len;
    memcpy(r->buf + off, s, first);
    if (len > first)
        memcpy(r->buf, s + first, len - first);
    r->head += len;
    return CONRING_QUEUED | was_empty;
}

/* Copy up to `max` of the oldest bytes into `out` and release them. When this
 * take leaves a latched ring EMPTY, *gap_lines receives how many lines were
 * dropped at this point of the stream (the notice belongs right after the
 * bytes just taken) and the ring accepts lines again; otherwise it is 0. */
CONRING_UNUSED static unsigned int conring_take(conring_t *r, char *out,
                                                unsigned int max,
                                                unsigned int *gap_lines)
{
    unsigned int used = conring_used(r);
    unsigned int n = used < max ? used : max;
    unsigned int off, first;

    if (n) {
        off = r->tail & (r->cap - 1);
        first = r->cap - off;
        if (first > n)
            first = n;
        memcpy(out, r->buf + off, first);
        if (n > first)
            memcpy(out + first, r->buf, n - first);
        r->tail += n;
    }
    *gap_lines = 0;
    if (r->latched && r->head == r->tail) {
        *gap_lines = r->gap_lines;
        r->gap_lines = 0;
        r->latched = 0;
    }
    return n;
}

#endif /* RETRO_CONRING_H */
