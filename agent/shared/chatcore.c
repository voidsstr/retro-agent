/*
 * chatcore.c - Chat-proxy state engine (SHARED, pure logic)
 *
 * See chatcore.h. Mirrors the hardware-proven behavior of the Windows
 * agent's chatproxy.c: single prompt slot, drop-oldest-half log ring,
 * status sequence counter. No OS calls, no locking — callers own both.
 */

#include "chatcore.h"
#include <stdlib.h>

void chatcore_init(chatcore_t *c, unsigned long log_max)
{
    memset(c, 0, sizeof(*c));
    c->log_max = log_max ? log_max : 4096;
}

void chatcore_free(chatcore_t *c)
{
    if (c->log) free(c->log);
    memset(c, 0, sizeof(*c));
}

int chatcore_prompt_push(chatcore_t *c, const char *text)
{
    unsigned long len;
    if (!text || !text[0]) return -1;
    len = (unsigned long)strlen(text);
    if (len >= CHATCORE_PROMPT_MAX) return -1;
    memcpy(c->prompt, text, len);
    c->prompt[len] = '\0';
    c->prompt_pending = 1;
    /* The slot now holds the NEW prompt; an unacknowledged older one can no
     * longer be put back (its text is gone, and the newer one wins anyway). */
    c->prompt_inflight = 0;
    c->prompt_owner = 0;
    return 0;
}

int chatcore_prompt_pop(chatcore_t *c, char *out, unsigned long outsz)
{
    unsigned long n;
    if (!c->prompt_pending || outsz == 0) return 0;
    n = (unsigned long)strlen(c->prompt);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, c->prompt, n);
    out[n] = '\0';
    c->prompt_pending = 0;
    c->prompt_inflight = 0;
    c->prompt_owner = 0;
    c->prompt[0] = '\0';
    return 1;
}

int chatcore_prompt_take(chatcore_t *c, char *out, unsigned long outsz,
                         unsigned long owner)
{
    unsigned long n;
    if (!c->prompt_pending || outsz == 0) return 0;
    n = (unsigned long)strlen(c->prompt);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, c->prompt, n);
    out[n] = '\0';
    c->prompt_pending = 0;
    /* keep the text: it is what a requeue puts back */
    c->prompt_inflight = owner ? 1 : 0;
    c->prompt_owner = owner;
    if (!owner) c->prompt[0] = '\0';
    return 1;
}

void chatcore_prompt_ack(chatcore_t *c, unsigned long owner)
{
    if (!owner || !c->prompt_inflight || c->prompt_owner != owner) return;
    c->prompt_inflight = 0;
    c->prompt_owner = 0;
    if (!c->prompt_pending) c->prompt[0] = '\0';
}

int chatcore_prompt_requeue(chatcore_t *c, unsigned long owner)
{
    if (!owner || !c->prompt_inflight || c->prompt_owner != owner) return 0;
    c->prompt_inflight = 0;
    c->prompt_owner = 0;
    if (c->prompt_pending) return 0;      /* a newer prompt is waiting: it wins */
    c->prompt_pending = 1;
    return 1;
}

unsigned long chatcore_log_append(chatcore_t *c, const char *text,
                                  unsigned long len)
{
    unsigned long new_size;

    if (len == 0) return 0;

    new_size = c->log_size + len;
    if (new_size > c->log_max) {
        /* Drop oldest half when full (same policy as chatproxy.c). log_base
         * advances by exactly what is dropped, so absolute reader offsets
         * keep pointing at the same bytes. */
        unsigned long keep = c->log_max / 2;
        if (c->log_size > keep) {
            unsigned long drop = c->log_size - keep;
            memmove(c->log, c->log + drop, keep);
            c->log_size = keep;
            c->log_base += drop;
        }
        new_size = c->log_size + len;
        if (new_size > c->log_max) {
            len = c->log_max - c->log_size;
            new_size = c->log_max;
        }
    }

    if (new_size > c->log_capacity) {
        unsigned long new_cap = c->log_capacity ? c->log_capacity * 2 : 4096;
        char *nb;
        while (new_cap < new_size) new_cap *= 2;
        if (new_cap > c->log_max) new_cap = c->log_max;
        nb = (char *)realloc(c->log, new_cap);
        if (!nb) return 0;
        c->log = nb;
        c->log_capacity = new_cap;
    }

    memcpy(c->log + c->log_size, text, len);
    c->log_size = new_size;
    return len;
}

unsigned long chatcore_log_append_once(chatcore_t *c, unsigned long id,
                                       const char *text, unsigned long len,
                                       int *dup)
{
    unsigned int i;
    unsigned long n;

    if (dup) *dup = 0;
    for (i = 0; i < c->dedup_n; i++) {
        if (c->dedup_id[i] == id) {
            if (dup) *dup = 1;
            return 0;
        }
    }
    n = chatcore_log_append(c, text, len);
    if (n > 0 || len == 0) {
        c->dedup_id[c->dedup_pos] = id;
        c->dedup_pos = (c->dedup_pos + 1) % CHATCORE_DEDUP_IDS;
        if (c->dedup_n < CHATCORE_DEDUP_IDS) c->dedup_n++;
    }
    return n;
}

unsigned long chatcore_log_end(const chatcore_t *c)
{
    return c->log_base + c->log_size;
}

unsigned long chatcore_log_window(const chatcore_t *c, unsigned long offset,
                                  unsigned long *avail, int *past_end)
{
    unsigned long end = c->log_base + c->log_size;

    if (past_end) *past_end = 0;
    if (offset > end) {
        /* Beyond the end: the log was cleared (or the agent restarted). */
        if (past_end) *past_end = 1;
        if (avail) *avail = 0;
        return c->log_size;
    }
    if (offset < c->log_base) offset = c->log_base;   /* dropped: skip ahead */
    if (avail) *avail = end - offset;
    return offset - c->log_base;
}

void chatcore_log_clear(chatcore_t *c)
{
    c->log_size = 0;
    c->log_base = 0;       /* absolute numbering restarts: see chatcore.h */
    c->prompt_pending = 0;
    c->prompt_inflight = 0;
    c->prompt_owner = 0;
    c->prompt[0] = '\0';
    c->status[0] = '\0';
    c->status_seq++;
}

void chatcore_status_set(chatcore_t *c, const char *text)
{
    unsigned long len;
    if (!text) text = "";
    len = (unsigned long)strlen(text);
    if (len >= CHATCORE_STATUS_MAX) len = CHATCORE_STATUS_MAX - 1;
    memcpy(c->status, text, len);
    c->status[len] = '\0';
    c->status_seq++;
}

/* append s to out (bounded); returns the new length */
static unsigned long chatcore_cat(char *out, unsigned long outsz,
                                  unsigned long pos, const char *s)
{
    while (*s && pos + 1 < outsz) out[pos++] = *s++;
    if (outsz) out[pos < outsz ? pos : outsz - 1] = '\0';
    return pos;
}

void chatcore_push_reply(char *out, unsigned long outsz, int replaced,
                         int listening, unsigned long idle_ms)
{
    unsigned long pos = 0;
    if (!outsz) return;
    out[0] = '\0';
    pos = chatcore_cat(out, outsz, pos, "OK");
    if (replaced)
        pos = chatcore_cat(out, outsz, pos, " replaced");
    if (!listening) {
        char num[12];
        unsigned long secs = idle_ms / 1000UL;
        int i = (int)sizeof(num) - 1;
        num[i] = '\0';
        do {
            num[--i] = (char)('0' + (int)(secs % 10UL));
            secs /= 10UL;
        } while (secs && i > 0);
        pos = chatcore_cat(out, outsz, pos, " no-listener ");
        pos = chatcore_cat(out, outsz, pos, num + i);
        pos = chatcore_cat(out, outsz, pos, "s");
    }
}
