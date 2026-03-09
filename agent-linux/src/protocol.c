/*
 * protocol.c - Frame I/O, auth, and discovery for Linux agent.
 * Same wire protocol as Windows agent, using POSIX sockets.
 */

#include "protocol.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/utsname.h>

static int recv_exact(SOCKET sock, char *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = recv(sock, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int send_exact(SOCKET sock, const char *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

int frame_recv(SOCKET sock, char **out_buf, uint32_t *out_len)
{
    unsigned char hdr[4];
    uint32_t payload_len;
    char *buf;

    if (recv_exact(sock, (char *)hdr, 4) != 0)
        return -1;

    payload_len = (uint32_t)hdr[0]
                | ((uint32_t)hdr[1] << 8)
                | ((uint32_t)hdr[2] << 16)
                | ((uint32_t)hdr[3] << 24);

    if (payload_len > MAX_FRAME_SIZE) {
        log_msg(LOG_PROTO, "frame_recv: payload too large (%u bytes)", payload_len);
        return -1;
    }

    buf = (char *)malloc(payload_len + 1);
    if (!buf) return -1;

    if (payload_len > 0) {
        if (recv_exact(sock, buf, (int)payload_len) != 0) {
            free(buf);
            return -1;
        }
    }
    buf[payload_len] = '\0';

    log_msg(LOG_PROTO, "frame_recv: %u bytes", payload_len);

    *out_buf = buf;
    *out_len = payload_len;
    return 0;
}

int frame_send(SOCKET sock, const char *data, uint32_t len)
{
    unsigned char hdr[4];

    log_msg(LOG_PROTO, "frame_send: %u bytes", len);

    hdr[0] = (unsigned char)(len & 0xFF);
    hdr[1] = (unsigned char)((len >> 8) & 0xFF);
    hdr[2] = (unsigned char)((len >> 16) & 0xFF);
    hdr[3] = (unsigned char)((len >> 24) & 0xFF);

    if (send_exact(sock, (const char *)hdr, 4) != 0)
        return -1;
    if (len > 0) {
        if (send_exact(sock, data, (int)len) != 0)
            return -1;
    }
    return 0;
}

int send_text_response(SOCKET sock, const char *text)
{
    uint32_t text_len = (uint32_t)strlen(text);
    uint32_t total = 1 + text_len;
    char *buf = (char *)malloc(total);
    int ret;

    if (!buf) return -1;
    buf[0] = RESP_OK_TEXT;
    memcpy(buf + 1, text, text_len);
    ret = frame_send(sock, buf, total);
    free(buf);
    return ret;
}

int send_binary_response(SOCKET sock, const char *data, uint32_t len)
{
    uint32_t total = 1 + len;
    char *buf = (char *)malloc(total);
    int ret;

    if (!buf) return -1;
    buf[0] = RESP_OK_BINARY;
    memcpy(buf + 1, data, len);
    ret = frame_send(sock, buf, total);
    free(buf);
    return ret;
}

int send_error_response(SOCKET sock, const char *errmsg)
{
    uint32_t msg_len = (uint32_t)strlen(errmsg);
    uint32_t total = 1 + msg_len;
    char *buf = (char *)malloc(total);
    int ret;

    if (!buf) return -1;
    buf[0] = RESP_ERROR;
    memcpy(buf + 1, errmsg, msg_len);
    ret = frame_send(sock, buf, total);
    free(buf);
    return ret;
}

int auth_verify(SOCKET sock, const char *secret)
{
    char *buf = NULL;
    uint32_t len = 0;
    char hostname[256];
    char response[512];
    struct utsname uts;

    /* Receive auth frame */
    if (frame_recv(sock, &buf, &len) != 0)
        return -1;

    /* Expected: "AUTH <secret>" */
    if (len < 5 || strncmp(buf, "AUTH ", 5) != 0) {
        free(buf);
        send_error_response(sock, "ERR bad auth format");
        return -1;
    }

    if (strcmp(buf + 5, secret) != 0) {
        log_msg(LOG_PROTO, "AUTH: failed (bad secret)");
        free(buf);
        send_error_response(sock, "ERR auth failed");
        return -1;
    }
    free(buf);
    log_msg(LOG_PROTO, "AUTH: success");

    /* Build OK response with hostname and OS */
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';

    uname(&uts);

    snprintf(response, sizeof(response), "OK %s Linux_%s linux",
             hostname, uts.release);

    return send_text_response(sock, response);
}

void discovery_build_packet(char *buf, int bufsize, const char *hostname,
                            const char *ip, int port, const char *os_str,
                            const char *cpu_str, unsigned long ram_mb,
                            const char *os_family)
{
    snprintf(buf, bufsize, "RETRO|%s|%s|%d|%s|%s|%lu|%s",
             hostname, ip, port, os_str, cpu_str, ram_mb, os_family);
    buf[bufsize - 1] = '\0';
}
