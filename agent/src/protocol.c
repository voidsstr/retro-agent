#include "protocol.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

/*
 * Only a TRANSFER earns a frame line. A small frame is already visible as its
 * CMD line in main.c, and logging every one - two lines per chat long-poll,
 * once a second on Win9x - rotated the whole boot out of the log on .243
 * within two hours (2026-09-24). Framing errors are always logged.
 */
#define PROTO_LOG_MIN 1024

/*
 * Frame format (both directions):
 *   [uint32 LE: payload length] [payload bytes]
 */

/*
 * NO RECV OR SEND MAY WAIT FOREVER ONCE A FRAME HAS STARTED.
 *
 * recv() and send() used to be called bare. On Win9x SO_RCVTIMEO is not an
 * option (it crashes Win98 Winsock - see handle_client), and the agent there
 * is ONE thread serving every client: select() only promises that a single
 * byte is waiting, so a peer that vanished part-way through a frame (a
 * dropped Wi-Fi laptop, a daemon killed mid-send, a half-open connection)
 * left recv() blocked for good and the WHOLE AGENT froze with it - every
 * other client, the chat, auto-update's reachability, all of it, until a
 * person walked over. A peer that stopped READING did the same to send().
 *
 * So every recv inside a frame is preceded by select(readfds) and every send
 * by select(writefds), and a stall past the limit fails the connection. The
 * limits are per call, not per transfer: a large UPLOAD or SCREENSHOT that is
 * merely slow keeps making progress and is never cut off. select() is safe
 * on Win9x; it is what the multiplex loop is built on.
 *
 * The one wait that stays unbounded is for the FIRST byte of a command frame:
 * that is the idle gap between commands, which a connection may legitimately
 * sit in for minutes (and which the multiplex loop only enters once select()
 * has already said a byte is there).
 *
 * Sends are chunked so that a single call cannot block for long either: a
 * blocking send() of a multi-megabyte screenshot to a peer that has stopped
 * reading would otherwise wait inside Winsock however long the select said.
 */
#define RECV_STALL_MS   30000   /* silent this long mid-frame = gone */
#define SEND_STALL_MS   60000   /* cannot take ANY data for this long = gone */
#define SEND_CHUNK      8192    /* Win9x's default SO_SNDBUF */
#define COALESCE_MAX    65536   /* frames up to this size go out in ONE send */

/* select() on one socket: >0 ready, 0 timed out, <0 error */
static int sock_wait(SOCKET sock, int for_write, DWORD ms)
{
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)((ms % 1000) * 1000);
    return select(0, for_write ? NULL : &fds, for_write ? &fds : NULL,
                  NULL, &tv);
}

/* recv() up to len bytes; unless wait_ms is 0, give up (-1) if nothing
 * arrives within wait_ms. */
static int recv_some(SOCKET sock, char *buf, int len, DWORD wait_ms)
{
    if (wait_ms) {
        int r = sock_wait(sock, 0, wait_ms);
        if (r == 0) {
            log_msg(LOG_PROTO, "recv: peer silent for %lu s mid-frame - "
                    "dropping the connection", (unsigned long)(wait_ms / 1000));
            return -1;
        }
        if (r < 0) return -1;
    }
    return recv(sock, buf, len, 0);
}

/* first_ms bounds the wait for the FIRST byte (0 = unbounded: the idle gap
 * before a command); every later byte is bounded by RECV_STALL_MS. */
static int recv_exact(SOCKET sock, char *buf, int len, DWORD first_ms)
{
    int total = 0;
    while (total < len) {
        int n = recv_some(sock, buf + total, len - total,
                          total ? RECV_STALL_MS : first_ms);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int send_exact(SOCKET sock, const char *buf, int len)
{
    int total = 0;
    while (total < len) {
        int want = len - total;
        int n, r;
        if (want > SEND_CHUNK) want = SEND_CHUNK;
        r = sock_wait(sock, 1, SEND_STALL_MS);
        if (r == 0) {
            log_msg(LOG_PROTO, "send: peer took nothing for %d s - dropping "
                    "the connection", SEND_STALL_MS / 1000);
            return -1;
        }
        if (r < 0) return -1;
        n = send(sock, buf + total, want, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static void put_len(unsigned char *hdr, DWORD len)
{
    hdr[0] = (unsigned char)(len & 0xFF);
    hdr[1] = (unsigned char)((len >> 8) & 0xFF);
    hdr[2] = (unsigned char)((len >> 16) & 0xFF);
    hdr[3] = (unsigned char)((len >> 24) & 0xFF);
}

int frame_recv_timed(SOCKET sock, char **out_buf, DWORD *out_len,
                     DWORD first_byte_ms)
{
    unsigned char hdr[4];
    DWORD payload_len;
    char *buf;

    if (recv_exact(sock, (char *)hdr, 4, first_byte_ms) != 0)
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
        /* For large frames (>64KB), receive in chunks with progress */
        if (payload_len > 65536) {
            DWORD total = 0;
            int last_pct = -1;
            printf("  Receiving: %lu KB\n", (unsigned long)(payload_len / 1024));
            while (total < payload_len) {
                DWORD want = payload_len - total;
                int n;
                if (want > 65536) want = 65536;
                n = recv_some(sock, buf + total, (int)want, RECV_STALL_MS);
                if (n <= 0) {
                    HeapFree(GetProcessHeap(), 0, buf);
                    return -1;
                }
                total += (DWORD)n;
                {
                    int pct = (int)((total * 100UL) / payload_len);
                    pct = (pct / 5) * 5;
                    if (pct > last_pct) {
                        printf("  Receiving: %d%%\r", pct);
                        fflush(stdout);
                        last_pct = pct;
                    }
                }
            }
            printf("  Receiving: 100%% done              \n");
        } else {
            if (recv_exact(sock, buf, (int)payload_len, RECV_STALL_MS) != 0) {
                HeapFree(GetProcessHeap(), 0, buf);
                return -1;
            }
        }
    }
    buf[payload_len] = '\0';

    if (payload_len >= PROTO_LOG_MIN)
        log_msg(LOG_PROTO, "frame_recv: %lu bytes", (unsigned long)payload_len);

    *out_buf = buf;
    *out_len = payload_len;
    return 0;
}

int frame_recv(SOCKET sock, char **out_buf, DWORD *out_len)
{
    return frame_recv_timed(sock, out_buf, out_len, 0);
}

/* Send a frame whose buffer starts with 4 RESERVED bytes for the header:
 * the header is written in place, so the whole frame goes out as one
 * contiguous send and a small reply is one TCP segment. */
static int frame_send_reserved(SOCKET sock, char *buf, DWORD payload_len)
{
    if (payload_len >= PROTO_LOG_MIN)
        log_msg(LOG_PROTO, "frame_send: %lu bytes", (unsigned long)payload_len);
    put_len((unsigned char *)buf, payload_len);
    return send_exact(sock, buf, (int)(payload_len + 4));
}

int frame_send(SOCKET sock, const char *data, DWORD len)
{
    unsigned char hdr[4];

    /* Small frames: header + payload in ONE send. Two sends of a tiny frame
     * are two segments, and against a peer that has Nagle on, the second
     * waits out the other end's delayed ACK (~200 ms per round trip). */
    if (len <= COALESCE_MAX) {
        char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, len + 4);
        if (buf) {
            int ret;
            if (len) memcpy(buf + 4, data, len);
            ret = frame_send_reserved(sock, buf, len);
            HeapFree(GetProcessHeap(), 0, buf);
            return ret;
        }
    }

    if (len >= PROTO_LOG_MIN)
        log_msg(LOG_PROTO, "frame_send: %lu bytes", (unsigned long)len);

    put_len(hdr, len);
    if (send_exact(sock, (const char *)hdr, 4) != 0)
        return -1;
    if (len > 0) {
        if (send_exact(sock, data, (int)len) != 0)
            return -1;
    }
    return 0;
}

/* status byte + payload, framed, in one buffer and one send */
static int send_status_frame(SOCKET sock, unsigned char status,
                             const char *data, DWORD len)
{
    DWORD total = 1 + len;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, total + 4);
    int ret;

    if (!buf) return -1;
    buf[4] = (char)status;
    if (len) memcpy(buf + 5, data, len);
    ret = frame_send_reserved(sock, buf, total);
    HeapFree(GetProcessHeap(), 0, buf);
    return ret;
}

int send_text_response(SOCKET sock, const char *text)
{
    return send_status_frame(sock, RESP_OK_TEXT, text, (DWORD)strlen(text));
}

int send_binary_response(SOCKET sock, const char *data, DWORD len)
{
    return send_status_frame(sock, RESP_OK_BINARY, data, len);
}

int send_error_response(SOCKET sock, const char *errmsg)
{
    return send_status_frame(sock, RESP_ERROR, errmsg, (DWORD)strlen(errmsg));
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
    /* Field 8 ("ai=0/1") is optional for parsers; advertises that the
     * retro-infer engine is staged next to the agent (AI_HELLO for detail) */
    char exe[MAX_PATH + 32];
    char *p;
    int ai = 0;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    p = strrchr(exe, '\\');
    if (p) {
        strcpy(p + 1, "retro-infer.exe");
        ai = GetFileAttributesA(exe) != 0xFFFFFFFF;
    }
    _snprintf(buf, bufsize, "RETRO|%s|%s|%d|%s|%s|%lu|windows|ai=%d",
              hostname, ip, port, os_str, cpu_str, (unsigned long)ram_mb, ai);
    buf[bufsize - 1] = '\0';
}
