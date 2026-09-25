#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <winsock2.h>
#include <windows.h>

/* Protocol constants + response status bytes live in the shared header
 * (also used by the DOS combined agent+chat and the chat client). */
#include "../shared/frameproto.h"

/* Frame I/O: 4-byte LE length prefix + payload */
int  frame_recv(SOCKET sock, char **out_buf, DWORD *out_len);
/* frame_recv, but the wait for the FIRST byte is bounded too (0 = unbounded,
 * which is frame_recv). For a frame that must follow at once - UPLOAD's
 * payload - where waiting forever would freeze a Win9x agent. */
int  frame_recv_timed(SOCKET sock, char **out_buf, DWORD *out_len,
                      DWORD first_byte_ms);
#define FRAME_FOLLOW_MS  30000   /* how long a following frame may take to start */
int  frame_send(SOCKET sock, const char *data, DWORD len);

/* Response builders */
int  send_text_response(SOCKET sock, const char *text);
int  send_binary_response(SOCKET sock, const char *data, DWORD len);
int  send_error_response(SOCKET sock, const char *errmsg);

/* Auth */
int  auth_verify(SOCKET sock, const char *secret);

/* Discovery */
void discovery_build_packet(char *buf, int bufsize, const char *hostname,
                            const char *ip, int port, const char *os_str,
                            const char *cpu_str, DWORD ram_mb);

#endif /* PROTOCOL_H */
