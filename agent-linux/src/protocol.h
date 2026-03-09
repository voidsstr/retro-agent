#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>

/* POSIX socket type */
typedef int SOCKET;
#define INVALID_SOCKET  (-1)
#define SOCKET_ERROR    (-1)

/* Protocol constants - same as Windows agent */
#define AGENT_TCP_PORT      9898
#define AGENT_UDP_PORT      9899
#define MAX_FRAME_SIZE      (16 * 1024 * 1024)  /* 16MB max frame */
#define MAX_COMMAND_LEN     4096
#define AUTH_TIMEOUT_MS     10000
#define DISCOVERY_INTERVAL  30  /* seconds */
#define PHONEHOME_INTERVAL  60  /* seconds, default phone-home interval */
#define PHONEHOME_MAX_RESPONSE 4096  /* max HTTP response body */

/* Response status bytes */
#define RESP_OK_TEXT    0x00
#define RESP_OK_BINARY  0x01
#define RESP_ERROR      0xFF

/* Frame I/O: 4-byte LE length prefix + payload */
int  frame_recv(SOCKET sock, char **out_buf, uint32_t *out_len);
int  frame_send(SOCKET sock, const char *data, uint32_t len);

/* Response builders */
int  send_text_response(SOCKET sock, const char *text);
int  send_binary_response(SOCKET sock, const char *data, uint32_t len);
int  send_error_response(SOCKET sock, const char *errmsg);

/* Auth */
int  auth_verify(SOCKET sock, const char *secret);

/* Discovery */
void discovery_build_packet(char *buf, int bufsize, const char *hostname,
                            const char *ip, int port, const char *os_str,
                            const char *cpu_str, unsigned long ram_mb,
                            const char *os_family);

#endif /* PROTOCOL_H */
