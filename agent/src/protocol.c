#include "protocol.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

/*
 * Frame format (both directions):
 *   [uint32 LE: payload length] [payload bytes]
 */

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

int frame_recv(SOCKET sock, char **out_buf, DWORD *out_len)
{
    unsigned char hdr[4];
    DWORD payload_len;
    char *buf;

    if (recv_exact(sock, (char *)hdr, 4) != 0)
        return -1;

    payload_len = (DWORD)hdr[0]
                | ((DWORD)hdr[1] << 8)
                | ((DWORD)hdr[2] << 16)
                | ((DWORD)hdr[3] << 24);

    if (payload_len > MAX_FRAME_SIZE) {
        log_msg(LOG_PROTO, "frame_recv: bad header (decoded %lu bytes) - "
                "dropping connection (non-framed client?)",
                (unsigned long)payload_len);
        return -1;
    }

    buf = (char *)HeapAlloc(GetProcessHeap(), 0, payload_len + 1);
    if (!buf) return -1;

    if (payload_len > 0) {
        if (recv_exact(sock, buf, (int)payload_len) != 0) {
            HeapFree(GetProcessHeap(), 0, buf);
            return -1;
        }
    }
    buf[payload_len] = '\0';

    log_msg(LOG_PROTO, "frame_recv: %lu bytes", (unsigned long)payload_len);

    *out_buf = buf;
    *out_len = payload_len;
    return 0;
}

int frame_send(SOCKET sock, const char *data, DWORD len)
{
    unsigned char hdr[4];

    log_msg(LOG_PROTO, "frame_send: %lu bytes", (unsigned long)len);

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
    DWORD text_len = (DWORD)strlen(text);
    DWORD total = 1 + text_len;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, total);
    int ret;

    if (!buf) return -1;
    buf[0] = RESP_OK_TEXT;
    memcpy(buf + 1, text, text_len);
    ret = frame_send(sock, buf, total);
    HeapFree(GetProcessHeap(), 0, buf);
    return ret;
}

int send_binary_response(SOCKET sock, const char *data, DWORD len)
{
    DWORD total = 1 + len;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, total);
    int ret;

    if (!buf) return -1;
    buf[0] = RESP_OK_BINARY;
    memcpy(buf + 1, data, len);
    ret = frame_send(sock, buf, total);
    HeapFree(GetProcessHeap(), 0, buf);
    return ret;
}

int send_error_response(SOCKET sock, const char *errmsg)
{
    DWORD msg_len = (DWORD)strlen(errmsg);
    DWORD total = 1 + msg_len;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, total);
    int ret;

    if (!buf) return -1;
    buf[0] = RESP_ERROR;
    memcpy(buf + 1, errmsg, msg_len);
    ret = frame_send(sock, buf, total);
    HeapFree(GetProcessHeap(), 0, buf);
    return ret;
}

int auth_verify(SOCKET sock, const char *secret)
{
    char *buf = NULL;
    DWORD len = 0;
    char hostname[256];
    char response[512];
    OSVERSIONINFOA osvi;

    /* Receive auth frame */
    if (frame_recv(sock, &buf, &len) != 0)
        return -1;

    /* Expected: "AUTH <secret>" */
    if (len < 5 || strncmp(buf, "AUTH ", 5) != 0) {
        HeapFree(GetProcessHeap(), 0, buf);
        send_error_response(sock, "ERR bad auth format");
        return -1;
    }

    if (strcmp(buf + 5, secret) != 0) {
        log_msg(LOG_PROTO, "AUTH: failed (bad secret)");
        HeapFree(GetProcessHeap(), 0, buf);
        send_error_response(sock, "ERR auth failed");
        return -1;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    log_msg(LOG_PROTO, "AUTH: success");

    /* Build OK response with hostname and OS */
    {
        DWORD hn_size = sizeof(hostname);
        GetComputerNameA(hostname, &hn_size);
    }

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);

    _snprintf(response, sizeof(response), "OK %s Win%lu.%lu",
              hostname, osvi.dwMajorVersion, osvi.dwMinorVersion);

    return send_text_response(sock, response);
}

void discovery_build_packet(char *buf, int bufsize, const char *hostname,
                            const char *ip, int port, const char *os_str,
                            const char *cpu_str, DWORD ram_mb)
{
    _snprintf(buf, bufsize, "RETRO|%s|%s|%d|%s|%s|%lu|windows",
              hostname, ip, port, os_str, cpu_str, (unsigned long)ram_mb);
    buf[bufsize - 1] = '\0';
}
