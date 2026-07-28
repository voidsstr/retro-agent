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
    c->prompt[0] = '\0';
    return 1;
}

unsigned long chatcore_log_append(chatcore_t *c, const char *text,
                                  unsigned long len)
{
    unsigned long new_size;

    if (len == 0) return 0;

    new_size = c->log_size + len;
    if (new_size > c->log_max) {
        /* Drop oldest half when full (same policy as chatproxy.c) */
        unsigned long keep = c->log_max / 2;
        if (c->log_size > keep) {
            memmove(c->log, c->log + (c->log_size - keep), keep);
            c->log_size = keep;
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

void chatcore_log_clear(chatcore_t *c)
{
    c->log_size = 0;
    c->prompt_pending = 0;
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
