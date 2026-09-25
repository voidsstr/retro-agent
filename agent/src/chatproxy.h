/*
 * chatproxy.h - connection hooks and long-poll PARKING for the chat bus.
 *
 * The command handlers themselves (PROMPT_*, LOG_*, STATUS_*, PROXY_*) are
 * declared in handlers.h like every other command. This header is the part
 * main.c drives: what happens around a connection rather than inside one
 * command.
 *
 * ---------------------------------------------------------------------------
 * PARKING (multiplex mode = every Win9x box)
 *
 * In multiplex mode ONE thread serves every client. A long-poll that BLOCKS
 * there waits for an event only a handler on that same thread can set, so it
 * can never be woken early: every poller cost the whole agent a full sleep,
 * pollers serialised into back-to-back 1 s stalls (the old clamp), and every
 * other command queued behind them.
 *
 * So in multiplex mode a long-poll whose condition is not met yet is PARKED:
 * the handler records {kind, arg, deadline} in the connection's chat_park_t
 * and returns WITHOUT replying. main.c then calls chatproxy_park_service()
 * after every command and every select() pass, which answers each parked poll
 * whose condition or deadline has been met, and bounds select()'s timeout by
 * the nearest deadline (chatproxy_park_ms_left). The DOS agent has always
 * worked this way (doschat.cpp, service_longpolls).
 *
 * Threaded mode (NT) keeps blocking waits: each client has its own thread.
 * ---------------------------------------------------------------------------
 */

#ifndef CHATPROXY_H
#define CHATPROXY_H

#include <winsock2.h>
#include <windows.h>

#define CHAT_PARK_NONE    0
#define CHAT_PARK_LOG     1
#define CHAT_PARK_PROMPT  2
#define CHAT_PARK_STATUS  3

typedef struct {
    int   kind;       /* CHAT_PARK_* */
    DWORD arg;        /* LOG: reader offset; STATUS: last seq */
    DWORD deadline;   /* GetTickCount() at which it is answered regardless */
} chat_park_t;

/* Initialise the chat state. main.c calls this on the accepting thread for
 * every new connection, so the one-time init happens before any client
 * thread can exist and is never raced. Idempotent and cheap. */
void chatproxy_init(void);

/* Multiplex mode: set the parking slot of the connection whose command is
 * about to be handled (NULL afterwards). While a slot is set, LOG_WAIT,
 * PROMPT_WAIT and STATUS_WAIT park instead of blocking. */
void chatproxy_set_park_slot(chat_park_t *slot);

/* Answer `p` if its condition or deadline is met (force = answer now, e.g.
 * the client sent another command while parked). Returns
 *    0  still parked
 *    1  answered (p is cleared)
 *   -1  the peer has gone: a pending prompt was left for someone else and
 *       the caller should drop the connection (p is cleared) */
int   chatproxy_park_service(SOCKET sock, chat_park_t *p, DWORD now, int force);

/* ms until `p` must be answered regardless (0 = due now); INFINITE if idle */
DWORD chatproxy_park_ms_left(const chat_park_t *p, DWORD now);

/* Forget a park without answering it (the connection is going away). */
void  chatproxy_park_clear(chat_park_t *p);

/* Every command a connection sends passes here first: it acknowledges the
 * prompt that connection took with its previous PROMPT_WAIT/PROMPT_POP. */
void  chatproxy_conn_command(SOCKET sock);

/* A connection is closing (call BEFORE closesocket, so the handle value
 * cannot have been reused yet). A prompt it took but never acknowledged is
 * put back for the next poller. */
void  chatproxy_conn_closed(SOCKET sock);

#endif /* CHATPROXY_H */
