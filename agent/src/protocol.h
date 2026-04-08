#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <winsock2.h>
#include <windows.h>

/* Protocol constants */
#define AGENT_TCP_PORT      9898
#define AGENT_TCP_PORT_ALT  9897  /* Secondary port for direct script access */
#define AGENT_UDP_PORT      9899
#define MAX_FRAME_SIZE      (32 * 1024 * 1024)  /* 32MB max frame */
#define MAX_COMMAND_LEN     4096
#define AUTH_TIMEOUT_MS     10000
#define DISCOVERY_INTERVAL  30000  /* 30 seconds */

/* Response status bytes */
#define RESP_OK_TEXT    0x00
#define RESP_OK_BINARY  0x01
#define RESP_ERROR      0xFF

/* Frame I/O: 4-byte LE length prefix + payload */
int  frame_recv(SOCKET sock, char **out_buf, DWORD *out_len);
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
