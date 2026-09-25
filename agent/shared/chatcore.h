/*
 * chatcore.h - Chat-proxy state engine (SHARED, pure logic, no OS calls)
 *
 * The single prompt slot, growable response log, and status sequence that
 * implement the retro chat bus:
 *
 *   UI side:     prompt push, log read (from offset), status read
 *   daemon side: prompt take/ack, log append, status set
 *
 * Used by:
 *   - the DOS combined agent+chat (agent/doschat) — single-threaded, calls
 *     directly with no locking
 *   - the Windows agent's chatproxy.c — wraps every call in its critical
 *     section and layers the long-poll waiting / parking on top
 *
 * All functions are non-blocking; long-poll semantics (LOG_WAIT etc.) are
 * the caller's job: check, then wait for a change signal or poll.
 *
 * ---------------------------------------------------------------------------
 * LOG OFFSETS ARE ABSOLUTE (log_base).
 *
 * The log is a ring that drops its oldest half when full. Readers used to
 * address it by BUFFER offset, so every drop moved the bytes under them: the
 * total a reader was told shrank, retro_chat concluded the log had been
 * cleared, reset to 0 and reprinted up to 128 KB, and the DOS UI (whose
 * offset was now past the end) showed nothing until the log grew back past
 * it. Now log[0] sits at absolute offset log_base, which advances by exactly
 * what a drop discards, and every offset on the wire is absolute:
 *
 *   reader offset <  log_base  its bytes were dropped: it resumes at log_base
 *   log_base <= offset <= end  normal: it gets [offset, end)
 *   offset >  end              the log was CLEARED (or the agent restarted):
 *                              it gets nothing and resets to 0
 *
 * end = log_base + log_size is what LOG_READ/LOG_WAIT report as "total", and
 * it only ever moves forward until a clear. A clear resets log_base to 0, so
 * a client that counts offset += bytes-received (every retro_chat before
 * this change) stays exactly in step with the absolute numbering.
 * ---------------------------------------------------------------------------
 */

#ifndef CHATCORE_H
#define CHATCORE_H

#include <string.h>

#define CHATCORE_PROMPT_MAX  8192
#define CHATCORE_STATUS_MAX  512
#define CHATCORE_DEDUP_IDS   16     /* LOG_APPEND2 ids remembered */

/* No PROMPT_WAIT for this long (and none parked/blocked right now) means
 * nobody is polling this box for prompts: PROMPT_PUSH says so. The daemon
 * re-issues PROMPT_WAIT the moment the previous one returns, so a live
 * listener is essentially never absent for more than a few seconds. */
#define CHATCORE_LISTENER_STALE_MS  15000UL

typedef struct {
    /* pending prompt slot (single in-flight prompt, like Claude Code).
     *
     * prompt_pending   1 = waiting to be taken
     * prompt_inflight  1 = taken by connection prompt_owner but not yet
     *                  acknowledged (that connection has not sent another
     *                  command). The text stays in prompt[] so it can be put
     *                  back if the taker's connection dies. */
    char prompt[CHATCORE_PROMPT_MAX];
    int  prompt_pending;
    int  prompt_inflight;
    unsigned long prompt_owner;

    /* growable response log (see "LOG OFFSETS ARE ABSOLUTE" above) */
    char *log;
    unsigned long log_size;
    unsigned long log_capacity;
    unsigned long log_max;          /* cap; oldest half dropped when full */
    unsigned long log_base;         /* absolute offset of log[0] */

    /* subagent status line + change sequence */
    char status[CHATCORE_STATUS_MAX];
    unsigned long status_seq;

    /* LOG_APPEND2 idempotency: the last CHATCORE_DEDUP_IDS chunk ids */
    unsigned long dedup_id[CHATCORE_DEDUP_IDS];
    unsigned int  dedup_n;          /* valid entries */
    unsigned int  dedup_pos;        /* next slot to overwrite */
} chatcore_t;

/* Initialize; log_max caps the response log (e.g. 256KB on NT, 16KB on DOS) */
void chatcore_init(chatcore_t *c, unsigned long log_max);
void chatcore_free(chatcore_t *c);

/* UI side. push returns 0 ok, -1 too long/empty. A push overwrites a prompt
 * that is still pending (check c->prompt_pending first to know), and ends any
 * in-flight one: a newer prompt supersedes an unacknowledged older one. */
int  chatcore_prompt_push(chatcore_t *c, const char *text);

/* daemon side, legacy. pop returns 1 and copies into out (size outsz) if a
 * prompt was pending, else 0. The prompt is gone for good once popped. */
int  chatcore_prompt_pop(chatcore_t *c, char *out, unsigned long outsz);

/* daemon side, at-least-once. take is pop, except the prompt stays IN FLIGHT
 * for `owner` (any nonzero id naming the taking connection) until:
 *   chatcore_prompt_ack(owner)      that connection sent its next command,
 *                                   so it demonstrably got the reply: done
 *   chatcore_prompt_requeue(owner)  that connection died first: the prompt
 *                                   is pending again (returns 1), unless a
 *                                   newer prompt was pushed meanwhile (0) */
int  chatcore_prompt_take(chatcore_t *c, char *out, unsigned long outsz,
                          unsigned long owner);
void chatcore_prompt_ack(chatcore_t *c, unsigned long owner);
int  chatcore_prompt_requeue(chatcore_t *c, unsigned long owner);

/* append returns bytes actually appended (may truncate at log_max). */
unsigned long chatcore_log_append(chatcore_t *c, const char *text,
                                  unsigned long len);

/* LOG_APPEND2: append unless chunk `id` was already appended recently.
 * Returns bytes appended; *dup = 1 (and nothing appended) for a repeat. The
 * id is only remembered once the append succeeded, so a failed append can be
 * retried. */
unsigned long chatcore_log_append_once(chatcore_t *c, unsigned long id,
                                       const char *text, unsigned long len,
                                       int *dup);

/* absolute end of the log (what LOG_READ/LOG_WAIT report as the total) */
unsigned long chatcore_log_end(const chatcore_t *c);

/* Resolve a reader's ABSOLUTE offset. Returns the index into c->log where its
 * unread bytes start and sets *avail to how many there are. *past_end = 1 when
 * the offset is beyond the end (log cleared / agent restarted): avail is 0 and
 * the reader should reset to 0. A reader behind log_base is moved up to it. */
unsigned long chatcore_log_window(const chatcore_t *c, unsigned long offset,
                                  unsigned long *avail, int *past_end);

void chatcore_log_clear(chatcore_t *c);

/* status set always bumps status_seq (even for an identical string). */
void chatcore_status_set(chatcore_t *c, const char *text);

/* The PROMPT_PUSH reply. Always starts "OK" (old clients test only for a
 * non-error status), then optional flags:
 *   "replaced"          an earlier prompt was still waiting and is gone
 *   "no-listener <n>s"  nobody is polling for prompts; the last PROMPT_WAIT
 *                       was n seconds ago (or the proxy started n s ago) */
void chatcore_push_reply(char *out, unsigned long outsz, int replaced,
                         int listening, unsigned long idle_ms);

#endif /* CHATCORE_H */
