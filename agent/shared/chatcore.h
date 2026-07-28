/*
 * chatcore.h - Chat-proxy state engine (SHARED, pure logic, no OS calls)
 *
 * The single prompt slot, growable response log, and status sequence that
 * implement the retro chat bus:
 *
 *   UI side:     prompt push, log read (from offset), status read
 *   daemon side: prompt pop, log append, status set
 *
 * Used by:
 *   - the DOS combined agent+chat (agent/doschat) — single-threaded, calls
 *     directly with no locking
 *   - the Windows agent's chatproxy.c — wraps every call in its critical
 *     section and layers the event-based long-poll waiting on top
 *
 * All functions are non-blocking; long-poll semantics (LOG_WAIT etc.) are
 * the caller's job: check, then wait for a change signal or poll.
 */

#ifndef CHATCORE_H
#define CHATCORE_H

#include <string.h>

#define CHATCORE_PROMPT_MAX  8192
#define CHATCORE_STATUS_MAX  512

typedef struct {
    /* pending prompt slot (single in-flight prompt, like Claude Code) */
    char prompt[CHATCORE_PROMPT_MAX];
    int  prompt_pending;

    /* growable response log */
    char *log;
    unsigned long log_size;
    unsigned long log_capacity;
    unsigned long log_max;          /* cap; oldest half dropped when full */

    /* subagent status line + change sequence */
    char status[CHATCORE_STATUS_MAX];
    unsigned long status_seq;
} chatcore_t;

/* Initialize; log_max caps the response log (e.g. 256KB on NT, 16KB on DOS) */
void chatcore_init(chatcore_t *c, unsigned long log_max);
void chatcore_free(chatcore_t *c);

/* UI side. push returns 0 ok, -1 too long/empty. */
int  chatcore_prompt_push(chatcore_t *c, const char *text);

/* daemon side. pop returns 1 and copies into out (size outsz) if a prompt
 * was pending, else 0. */
int  chatcore_prompt_pop(chatcore_t *c, char *out, unsigned long outsz);

/* append returns bytes actually appended (may truncate at log_max). */
unsigned long chatcore_log_append(chatcore_t *c, const char *text,
                                  unsigned long len);
void chatcore_log_clear(chatcore_t *c);

/* status set always bumps status_seq (even for an identical string). */
void chatcore_status_set(chatcore_t *c, const char *text);

#endif /* CHATCORE_H */
