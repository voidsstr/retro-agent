/*
 * frameproto.h - Retro agent wire-protocol constants (SHARED)
 *
 * Single source of truth for the framed TCP protocol spoken by:
 *   - the Windows agent (agent/src/protocol.h includes this)
 *   - the Windows chat client (agent/tools/retro_chat.c)
 *   - the DOS combined agent+chat (agent/doschat/doschat.cpp)
 *   - (the Python client mirrors these in client/retro_protocol.py)
 *
 * Frame format (both directions): [uint32 LE payload length][payload].
 * First client frame must be "AUTH <secret>"; every response starts with
 * a status byte (RESP_*) followed by the payload.
 */

#ifndef FRAMEPROTO_H
#define FRAMEPROTO_H

/* Ports */
#define AGENT_TCP_PORT      9898
#define AGENT_TCP_PORT_ALT  9897  /* Secondary port for direct script access */
#define AGENT_UDP_PORT      9899  /* UDP discovery broadcast */

/* Limits */
#define MAX_FRAME_SIZE      (32 * 1024 * 1024)  /* 32MB max frame (NT agent) */
#define MAX_COMMAND_LEN     4096
#define AUTH_TIMEOUT_MS     10000
#define DISCOVERY_INTERVAL  30000  /* ms between discovery broadcasts */

/* Response status bytes */
#define RESP_OK_TEXT    0x00
#define RESP_OK_BINARY  0x01
#define RESP_ERROR      0xFF

/* Chat-proxy long-poll cap (LOG_WAIT/PROMPT_WAIT/STATUS_WAIT) */
#define CHAT_WAIT_MAX_TIMEOUT_MS  60000

#endif /* FRAMEPROTO_H */
