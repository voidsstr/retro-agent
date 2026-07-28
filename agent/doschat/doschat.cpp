/*
 * doschat.cpp - Combined retro AGENT + CHAT for DOS (Win98's DOS 7.1)
 *
 * One real-mode exe that does both halves of the retro chat system:
 *
 *   AGENT half: a framed-TCP protocol server on port 9898 (same wire
 *   protocol as the Windows retro_agent.exe — see agent/shared/frameproto.h)
 *   plus the UDP discovery broadcast on 9899, so the retro chat daemon
 *   claims a DOS box exactly like a Windows box. Supports the command
 *   subset that makes sense on DOS: PING, SYSINFO, DIRLIST, EXEC, UPLOAD,
 *   DOWNLOAD, DELETE, MKDIR, QUIT, REBOOT + the full chat-proxy surface
 *   (PROMPT_*, LOG_*, STATUS_*, PROXY_*).
 *
 *   CHAT half: the retro_chat-style console UI in the same process. On DOS
 *   there is no loopback connection — the UI talks straight to the shared
 *   chat state (agent/shared/chatcore.[ch], the SAME code the Windows
 *   agent's chatproxy.c uses). Response text is sanitized/wrapped by
 *   agent/shared/chattext.h (the SAME code retro_chat.exe uses).
 *
 * DOS is single-tasking, so everything runs in one cooperative loop:
 * drive the mTCP stack, service client sockets (with long-poll deadlines),
 * pump the keyboard, animate the spinner. Networking is mTCP (GPLv3,
 * Watcom) over any packet driver; load NE2000.COM (or the right driver)
 * + DHCP first, exactly like the DOSGAME network setup:
 *
 *   SET MTCPCFG=C:\DOSGAME\NET\MTCP.CFG
 *   LH NE2000.COM 0x60 <irq> <iobase>
 *   DHCP
 *   DOSCHAT
 *
 * Build: see Makefile (Open Watcom 16-bit, large model, links mTCP).
 */

#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <direct.h>
#include <io.h>
#include <conio.h>
#include <i86.h>
#include <malloc.h>

#include "types.h"
#include "trace.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "udp.h"
#include "dns.h"
#include "timer.h"
#include "tcp.h"
#include "tcpsockm.h"

#include "../shared/frameproto.h"
#include "../shared/chatcore.h"
#include "../shared/chatcore.c"
#include "../shared/chattext.h"

#ifndef DOSCHAT_VERSION
#define DOSCHAT_VERSION "0.1.0"
#endif

#define AGENT_SECRET   "retro-agent-secret"
/* The chat daemon alone holds three long-poll connections (prompt, log,
 * status) and reconnects without always closing the old one, so keep
 * comfortable headroom — a full table used to silently drop new clients,
 * which looked like an agent timeout. TCP_MAX_SOCKETS in doschat.cfg must
 * be >= MAX_CLIENTS + 1 (the listener). */
#define MAX_CLIENTS    7
#define FRAME_CAP      16384        /* DOS conventional-memory frame cap */
#define LOG_MAX_DOS    16384        /* chat log ring on DOS */
#define RECV_BUF_SIZE  4096
#define EXEC_TMP       "C:\\RCEXEC.TMP"

/* ---------------------------------------------------------------- clients */

enum pollwait_t { PW_NONE = 0, PW_LOG, PW_PROMPT, PW_STATUS };

typedef struct {
    TcpSocket *sock;
    int authed;
    /* incoming frame assembly */
    unsigned char hdr[4];
    int hdr_got;
    char *payload;
    unsigned long payload_len;
    unsigned long payload_got;
    /* two-frame UPLOAD: destination path captured from the command frame */
    char upload_path[128];
    int expecting_upload;
    /* long-poll state */
    pollwait_t pending;
    unsigned long p_param;          /* LOG: offset, STATUS: last_seq */
    clock_t p_deadline;
} client_t;

static client_t clients[MAX_CLIENTS];
static chatcore_t core;

/* ONE shared response scratch buffer. In the 16-bit large model the whole
 * _BSS segment caps at 64K, so several 16K statics don't fit; command
 * handlers are mutually exclusive (single-threaded cooperative loop), so
 * they take turns with this far-heap block. */
static char far *scratch = NULL;
#define SCRATCH_SIZE (FRAME_CAP + 64)

/* UI state */
static int ui_waiting = 0;              /* prompt sent, no response yet */
static unsigned long ui_log_shown = 0;  /* bytes of core.log already printed */
static unsigned long ui_status_seen = 0;
static volatile uint8_t CtrlBreakDetected = 0;

void __interrupt __far ctrlBreakHandler() { CtrlBreakDetected = 1; }
void __interrupt __far ctrlCHandler() {}

/* ---------------------------------------------------------------- console */

#define SCR_W 80
#define SCR_H 25
#define ROW_STATUS (SCR_H - 2)
#define ROW_INPUT  (SCR_H - 1)
#define SCROLL_BOT (SCR_H - 3)

#define ATTR_DEFAULT  0x0F
#define ATTR_RESPONSE 0x0B
#define ATTR_PROMPT   0x0D
#define ATTR_SPINNER  0x0E
#define ATTR_STATUS   0x0A
#define ATTR_BANNER   0x07

static unsigned short far *vram =
    (unsigned short far *)MK_FP(0xB800, 0);
static int cur_row = 0, cur_col = 0;    /* scroll-area cursor */

static void scroll_area(void)
{
    union REGS r;
    r.h.ah = 0x06; r.h.al = 1;                 /* scroll up 1 line */
    r.h.bh = ATTR_BANNER;
    r.w.cx = 0;                                /* top-left 0,0 */
    r.h.dh = SCROLL_BOT; r.h.dl = SCR_W - 1;
    int86(0x10, &r, &r);
}

static void put_char(char c, unsigned char attr)
{
    if (c == '\r') return;
    if (c == '\n' || cur_col >= SCR_W) {
        cur_col = 0;
        if (cur_row < SCROLL_BOT) cur_row++;
        else scroll_area();
        if (c == '\n') return;
    }
    vram[cur_row * SCR_W + cur_col] =
        ((unsigned short)attr << 8) | (unsigned char)c;
    cur_col++;
}

static void put_text(const char *s, unsigned long len, unsigned char attr)
{
    unsigned long i;
    for (i = 0; i < len; i++) put_char(s[i], attr);
}

static void put_line(const char *s, unsigned char attr)
{
    put_text(s, (unsigned long)strlen(s), attr);
    put_char('\n', attr);
}

static void fill_row(int row, unsigned char attr)
{
    int x;
    for (x = 0; x < SCR_W; x++)
        vram[row * SCR_W + x] = ((unsigned short)attr << 8) | ' ';
}

/* ---- input line ---- */

#define INPUT_MAX 256
static char in_buf[INPUT_MAX];
static int in_len = 0;

#define HIST_MAX 8
static char hist[HIST_MAX][INPUT_MAX];
static int hist_count = 0, hist_nav = 0;

static int spin_idx = 0;
static const char SPIN[] = "|/-\\";

static void draw_status_row(void)
{
    int x = 0;
    fill_row(ROW_STATUS, ATTR_BANNER);
    if (ui_waiting) {
        char sp[20];
        sprintf(sp, "* Working... %c ", SPIN[spin_idx & 3]);
        for (; sp[x] && x < 16; x++)
            vram[ROW_STATUS * SCR_W + x] =
                ((unsigned short)ATTR_SPINNER << 8) | sp[x];
    }
    if (core.status[0]) {
        const char *s = core.status;
        int i = 0;
        x = 17;
        vram[ROW_STATUS * SCR_W + x++] = ((unsigned short)ATTR_STATUS << 8) | '[';
        while (s[i] && x < SCR_W - 1)
            vram[ROW_STATUS * SCR_W + x++] =
                ((unsigned short)ATTR_STATUS << 8) | s[i++];
        vram[ROW_STATUS * SCR_W + x] = ((unsigned short)ATTR_STATUS << 8) | ']';
    }
}

static void draw_input_row(void)
{
    int x, show, start;
    fill_row(ROW_INPUT, ATTR_DEFAULT);
    vram[ROW_INPUT * SCR_W + 0] = ((unsigned short)ATTR_PROMPT << 8) | '>';
    show = SCR_W - 3;
    start = (in_len > show) ? in_len - show : 0;
    for (x = 0; x < in_len - start; x++)
        vram[ROW_INPUT * SCR_W + 2 + x] =
            ((unsigned short)ATTR_DEFAULT << 8) | in_buf[start + x];
    /* park the hardware cursor at the edit position */
    {
        union REGS r;
        r.h.ah = 0x02; r.h.bh = 0;
        r.h.dh = ROW_INPUT; r.h.dl = (unsigned char)(2 + (in_len - start));
        int86(0x10, &r, &r);
    }
}

/* print any not-yet-shown chat log content (shared sanitize+wrap) */
static void ui_pump_log(void)
{
    /* Far-heap scratch: everything static lands in DGROUP, and in the
     * 16-bit large model DGROUP must also leave room for mTCP's socket
     * mallocs — a few KB of statics here is what pushed initStack over
     * the edge and made the stack "fail to initialize". */
    char far *clean = scratch + FRAME_CAP / 2;   /* 8K half */
    char far *wrapped = scratch;                 /* 8K half, wrap output */
    const unsigned long clean_cap = 2048;
    while (core.log_size > ui_log_shown) {
        unsigned long chunk = core.log_size - ui_log_shown;
        unsigned long cl, wl;
        if (chunk > clean_cap - 2) chunk = clean_cap - 2;
        cl = chat_sanitize_chunk(core.log + ui_log_shown, chunk, clean);
        wl = chat_wrap_text(clean, cl, wrapped, SCR_W - 1);
        put_text(wrapped, wl, ATTR_RESPONSE);
        ui_log_shown += chunk;
        ui_waiting = 0;
    }
}

/* ------------------------------------------------------------ frame send */

static int send_all(TcpSocket *s, const char *data, unsigned long len)
{
    unsigned long sent = 0;
    clock_t give_up = clock() + 10 * CLOCKS_PER_SEC;
    while (sent < len) {
        int16_t rc = s->send((uint8_t *)(data + sent),
                             (uint16_t)((len - sent > 1400) ? 1400
                                                            : (len - sent)));
        if (rc > 0) { sent += rc; give_up = clock() + 10 * CLOCKS_PER_SEC; }
        else if (rc < 0) return -1;
        PACKET_PROCESS_SINGLE;
        Arp::driveArp();
        Tcp::drivePackets();
        if (clock() > give_up) return -1;
    }
    return 0;
}

static int frame_send_sock_hdr(TcpSocket *s, unsigned long len)
{
    unsigned char h[4];
    h[0] = (unsigned char)(len & 0xFF);
    h[1] = (unsigned char)((len >> 8) & 0xFF);
    h[2] = (unsigned char)((len >> 16) & 0xFF);
    h[3] = (unsigned char)((len >> 24) & 0xFF);
    return send_all(s, (char *)h, 4);
}

static int frame_send_sock(TcpSocket *s, const char *data, unsigned long len)
{
    if (frame_send_sock_hdr(s, len) != 0) return -1;
    return len ? send_all(s, data, len) : 0;
}

static int resp_status(TcpSocket *s, unsigned char status,
                       const char *data, unsigned long len)
{
    /* status byte then payload — two sends, no assembly buffer needed */
    char sb = (char)status;
    if (len > FRAME_CAP) return -1;
    if (frame_send_sock_hdr(s, len + 1) != 0) return -1;
    if (send_all(s, &sb, 1) != 0) return -1;
    return len ? send_all(s, data, len) : 0;
}

static int resp_text(TcpSocket *s, const char *text)
{
    return resp_status(s, RESP_OK_TEXT, text, (unsigned long)strlen(text));
}

static int resp_err(TcpSocket *s, const char *msg)
{
    return resp_status(s, RESP_ERROR, msg, (unsigned long)strlen(msg));
}

/* ------------------------------------------------------------- commands */

static void cmd_sysinfo(TcpSocket *s)
{
    char buf[400];
    union REGS r;
    unsigned kb;
    r.h.ah = 0x12;                   /* int 12h: base memory in KB */
    int86(0x12, &r, &r);
    kb = r.w.ax;
    sprintf(buf,
        "{\"hostname\":\"%s\",\"os\":\"DOS\",\"os_version\":\"7.10\","
        "\"agent_version\":\"%s-dos\",\"cpu\":\"x86\","
        "\"base_mem_kb\":%u,\"ip\":\"%d.%d.%d.%d\","
        "\"combined_chat\":1}",
        MyHostname, DOSCHAT_VERSION, kb,
        MyIpAddr[0], MyIpAddr[1], MyIpAddr[2], MyIpAddr[3]);
    resp_text(s, buf);
}

static void cmd_dirlist(TcpSocket *s, const char *path)
{
    char far *out = scratch;
    char pat[144];
    struct find_t ft;
    unsigned long used = 0;
    int first = 1, rc;

    if (!path || !path[0]) { resp_err(s, "DIRLIST requires path"); return; }
    sprintf(pat, "%s\\*.*", path);
    used += sprintf(out + used, "[");
    rc = _dos_findfirst(pat, _A_NORMAL | _A_SUBDIR, &ft);
    while (rc == 0 && used < sizeof(out) - 96) {
        used += sprintf(out + used,
            "%s{\"name\":\"%s\",\"size\":%lu,\"dir\":%d}",
            first ? "" : ",", ft.name, (unsigned long)ft.size,
            (ft.attrib & _A_SUBDIR) ? 1 : 0);
        first = 0;
        rc = _dos_findnext(&ft);
    }
    used += sprintf(out + used, "]");
    resp_status(s, RESP_OK_TEXT, out, used);
}

static void cmd_exec(TcpSocket *s, const char *cmdline)
{
    char far *out = scratch;
    char full[400];
    FILE *f;
    unsigned long n = 0;

    if (!cmdline || !cmdline[0]) { resp_err(s, "EXEC requires command"); return; }
    /* NOTE: while system() runs, the TCP stack is not serviced. Keep
     * commands short; the daemon's timeout budget covers ~60s. */
    sprintf(full, "%s > %s", cmdline, EXEC_TMP);
    system(full);
    f = fopen(EXEC_TMP, "rb");
    if (f) {
        n = fread(out, 1, sizeof(out) - 1, f);
        fclose(f);
        unlink(EXEC_TMP);
    }
    resp_status(s, RESP_OK_TEXT, out, n);
}

static void cmd_download(TcpSocket *s, const char *path)
{
    char far *out = scratch;
    FILE *f;
    unsigned long n;
    if (!path || !path[0]) { resp_err(s, "DOWNLOAD requires path"); return; }
    f = fopen(path, "rb");
    if (!f) { resp_err(s, "Cannot open file"); return; }
    n = fread(out, 1, sizeof(out), f);
    if (!feof(f)) {
        fclose(f);
        resp_err(s, "File too large for DOS agent (16KB frame cap)");
        return;
    }
    fclose(f);
    resp_status(s, RESP_OK_BINARY, out, n);
}

/* chat-proxy long-poll answers */
static void answer_log_read(TcpSocket *s, unsigned long offset)
{
    char far *out = scratch;
    unsigned long hl, used;
    if (offset > core.log_size) offset = core.log_size;
    hl = sprintf(out, "%lu\n", core.log_size);
    used = core.log_size - offset;
    memcpy(out + hl, core.log + offset, used);
    resp_status(s, RESP_OK_TEXT, out, hl + used);
}

static void answer_status(TcpSocket *s)
{
    char out[CHATCORE_STATUS_MAX + 16];
    sprintf(out, "%lu\n%s", core.status_seq, core.status);
    resp_text(s, out);
}

static void answer_prompt(TcpSocket *s)
{
    char out[CHATCORE_PROMPT_MAX];
    if (chatcore_prompt_pop(&core, out, sizeof(out)))
        resp_text(s, out);
    else
        resp_text(s, "");
}

static clock_t poll_deadline(const char *args, unsigned long defms)
{
    /* args: "<num> [timeout_ms]" — timeout is the token after the space */
    unsigned long ms = defms;
    const char *sp = args ? strchr(args, ' ') : 0;
    if (sp) ms = strtoul(sp + 1, NULL, 10);
    if (ms == 0 || ms > CHAT_WAIT_MAX_TIMEOUT_MS)
        ms = CHAT_WAIT_MAX_TIMEOUT_MS;
    return clock() + (clock_t)((ms / 1000UL) * CLOCKS_PER_SEC)
                 + (clock_t)(((ms % 1000UL) * CLOCKS_PER_SEC) / 1000UL);
}

static void dispatch(client_t *c, char *cmd, unsigned long len)
{
    TcpSocket *s = c->sock;
    char *args;

    /* binary payload frame of a two-frame UPLOAD */
    if (c->expecting_upload) {
        FILE *f = fopen(c->upload_path, "wb");
        c->expecting_upload = 0;
        if (!f) { resp_err(s, "Cannot create file"); return; }
        fwrite(cmd, 1, len, f);
        fclose(f);
        resp_text(s, "OK");
        return;
    }

    if (!c->authed) {
        if (len > 5 && strncmp(cmd, "AUTH ", 5) == 0
            && strcmp(cmd + 5, AGENT_SECRET) == 0) {
            char ok[96];
            c->authed = 1;
            sprintf(ok, "OK %s DOS7.10", MyHostname);
            resp_text(s, ok);
        } else {
            resp_err(s, "ERR auth failed");
            s->close();
        }
        return;
    }

    args = strchr(cmd, ' ');
    if (args) *args++ = '\0';

    if (!strcmp(cmd, "PING"))            { resp_text(s, "PONG"); }
    else if (!strcmp(cmd, "SYSINFO"))    { cmd_sysinfo(s); }
    else if (!strcmp(cmd, "DIRLIST"))    { cmd_dirlist(s, args); }
    else if (!strcmp(cmd, "EXEC"))       { cmd_exec(s, args); }
    else if (!strcmp(cmd, "DOWNLOAD"))   { cmd_download(s, args); }
    else if (!strcmp(cmd, "DELETE"))     {
        if (args && unlink(args) == 0) resp_text(s, "OK");
        else resp_err(s, "Delete failed");
    }
    else if (!strcmp(cmd, "MKDIR"))      {
        if (args && mkdir(args) == 0) resp_text(s, "OK");
        else resp_err(s, "Mkdir failed");
    }
    else if (!strcmp(cmd, "UPLOAD"))     {
        if (!args || !args[0]) { resp_err(s, "UPLOAD requires path"); return; }
        strncpy(c->upload_path, args, sizeof(c->upload_path) - 1);
        c->upload_path[sizeof(c->upload_path) - 1] = '\0';
        c->expecting_upload = 1;   /* next frame is the payload */
    }
    else if (!strcmp(cmd, "PROMPT_PUSH")) {
        if (chatcore_prompt_push(&core, args ? args : "") == 0) {
            ui_waiting = 1;
            /* echo the remote prompt into the local scrollback */
            put_text("> ", 2, ATTR_PROMPT);
            put_line(args ? args : "", ATTR_DEFAULT);
            resp_text(s, "OK");
        } else resp_err(s, "PROMPT_PUSH requires text");
    }
    else if (!strcmp(cmd, "PROMPT_POP"))  { answer_prompt(s); }
    else if (!strcmp(cmd, "PROMPT_WAIT")) {
        if (core.prompt_pending) answer_prompt(s);
        else {
            c->pending = PW_PROMPT;
            c->p_deadline = poll_deadline(args ? args - 1 : 0, 30000);
        }
    }
    else if (!strcmp(cmd, "LOG_APPEND")) {
        chatcore_log_append(&core, args ? args : "",
                            args ? (unsigned long)strlen(args) : 0);
        resp_text(s, "OK");
    }
    else if (!strcmp(cmd, "LOG_READ")) {
        answer_log_read(s, args ? strtoul(args, NULL, 10) : 0);
    }
    else if (!strcmp(cmd, "LOG_WAIT")) {
        unsigned long off = args ? strtoul(args, NULL, 10) : 0;
        if (core.log_size > off || (off > 0 && core.log_size < off))
            answer_log_read(s, off);
        else {
            c->pending = PW_LOG;
            c->p_param = off;
            c->p_deadline = poll_deadline(args, 30000);
        }
    }
    else if (!strcmp(cmd, "LOG_CLEAR")) {
        chatcore_log_clear(&core);
        ui_log_shown = 0;
        ui_waiting = 0;
        resp_text(s, "OK");
    }
    else if (!strcmp(cmd, "STATUS_SET")) {
        chatcore_status_set(&core, args ? args : "");
        resp_text(s, "OK");
    }
    else if (!strcmp(cmd, "STATUS_GET")) { answer_status(s); }
    else if (!strcmp(cmd, "STATUS_WAIT")) {
        unsigned long seq = args ? strtoul(args, NULL, 10) : 0;
        if (core.status_seq != seq) answer_status(s);
        else {
            c->pending = PW_STATUS;
            c->p_param = seq;
            c->p_deadline = poll_deadline(args, 30000);
        }
    }
    else if (!strcmp(cmd, "PROXY_GET")) { resp_text(s, ""); }
    else if (!strcmp(cmd, "PROXY_SET")) { resp_text(s, "OK"); }
    else if (!strcmp(cmd, "QUIT"))      { resp_text(s, "OK"); s->close(); }
    else if (!strcmp(cmd, "REBOOT"))    {
        resp_text(s, "OK");
        outp(0x64, 0xFE);            /* keyboard-controller pulse reset */
    }
    else { resp_err(s, "Unknown command (DOS agent subset)"); }
}

/* -------------------------------------------------- client frame pump */

static void client_reset_frame(client_t *c)
{
    if (c->payload) free(c->payload);
    c->payload = NULL;
    c->hdr_got = 0;
    c->payload_len = c->payload_got = 0;
}

static void client_drop(client_t *c)
{
    if (c->sock) {
        c->sock->close();
        TcpSocketMgr::freeSocket(c->sock);
        c->sock = NULL;
    }
    client_reset_frame(c);
    c->authed = 0;
    c->pending = PW_NONE;
    c->expecting_upload = 0;
}

static void client_pump(client_t *c)
{
    static uint8_t rb[RECV_BUF_SIZE];
    int16_t got;

    if (!c->sock) return;
    if (c->sock->isRemoteClosed() && c->sock->recvDataWaiting() == 0) {
        client_drop(c);
        return;
    }

    for (;;) {
        if (c->hdr_got < 4) {
            got = c->sock->recv(rb, (uint16_t)(4 - c->hdr_got));
            if (got <= 0) break;
            memcpy(c->hdr + c->hdr_got, rb, got);
            c->hdr_got += got;
            if (c->hdr_got < 4) continue;
            c->payload_len = (unsigned long)c->hdr[0]
                           | ((unsigned long)c->hdr[1] << 8)
                           | ((unsigned long)c->hdr[2] << 16)
                           | ((unsigned long)c->hdr[3] << 24);
            if (c->payload_len > FRAME_CAP) {
                resp_err(c->sock, "Frame too large for DOS agent");
                client_drop(c);
                return;
            }
            c->payload = (char *)malloc(c->payload_len + 1);
            if (!c->payload) { client_drop(c); return; }
            c->payload_got = 0;
        }
        if (c->payload_got < c->payload_len) {
            unsigned long want = c->payload_len - c->payload_got;
            if (want > sizeof(rb)) want = sizeof(rb);
            got = c->sock->recv(rb, (uint16_t)want);
            if (got <= 0) break;
            memcpy(c->payload + c->payload_got, rb, got);
            c->payload_got += got;
        }
        if (c->hdr_got == 4 && c->payload_got == c->payload_len) {
            c->payload[c->payload_len] = '\0';
            dispatch(c, c->payload, c->payload_len);
            client_reset_frame(c);
            if (!c->sock) return;    /* dispatch may have dropped us */
        }
    }
}

/* answer matured long-polls (called whenever state may have changed) */
static void service_longpolls(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];
        if (!c->sock || c->pending == PW_NONE) continue;
        switch (c->pending) {
        case PW_LOG:
            if (core.log_size > c->p_param
                || (c->p_param > 0 && core.log_size < c->p_param)
                || clock() >= c->p_deadline) {
                c->pending = PW_NONE;
                answer_log_read(c->sock, c->p_param);
            }
            break;
        case PW_PROMPT:
            if (core.prompt_pending || clock() >= c->p_deadline) {
                c->pending = PW_NONE;
                answer_prompt(c->sock);
            }
            break;
        case PW_STATUS:
            if (core.status_seq != c->p_param || clock() >= c->p_deadline) {
                c->pending = PW_NONE;
                answer_status(c->sock);
            }
            break;
        default:
            break;
        }
    }
}

/* --------------------------------------------------------- discovery */

static void discovery_send(void)
{
    char pkt[200];
    IpAddr_t bcast = { 255, 255, 255, 255 };
    union REGS r;
    unsigned kb;
    r.h.ah = 0x12; int86(0x12, &r, &r); kb = r.w.ax;
    /* Same shape as the Windows agent's packet (protocol.c):
     * RETRO|host|ip|port|os|cpu|ram_mb|platform|ai=N */
    sprintf(pkt, "RETRO|%s|%d.%d.%d.%d|%d|DOS 7.10|x86|%u|dos|ai=0",
            MyHostname, MyIpAddr[0], MyIpAddr[1], MyIpAddr[2], MyIpAddr[3],
            AGENT_TCP_PORT, kb / 1024);
    Udp::sendUdp(bcast, AGENT_UDP_PORT, AGENT_UDP_PORT,
                 (uint16_t)strlen(pkt), (uint8_t *)pkt, 1);
}

/* ---------------------------------------------------------------- UI */

static void ui_submit(void)
{
    in_buf[in_len] = '\0';
    if (!in_len) return;
    if (!strcmp(in_buf, ":quit")) { CtrlBreakDetected = 1; return; }
    put_text("> ", 2, ATTR_PROMPT);
    put_line(in_buf, ATTR_DEFAULT);
    if (!strcmp(in_buf, ":clear")) {
        chatcore_log_clear(&core);
        ui_log_shown = 0; ui_waiting = 0;
    } else {
        chatcore_prompt_push(&core, in_buf);
        ui_waiting = 1;
        if (hist_count == 0 || strcmp(hist[hist_count - 1], in_buf)) {
            if (hist_count == HIST_MAX) {
                memmove(hist[0], hist[1], sizeof(hist[0]) * (HIST_MAX - 1));
                hist_count--;
            }
            strcpy(hist[hist_count++], in_buf);
        }
    }
    hist_nav = hist_count;
    in_len = 0;
}

static void ui_key(uint16_t key)
{
    unsigned char ch = key & 0xFF;
    unsigned char scan = key >> 8;

    if (ch == 27) { CtrlBreakDetected = 1; return; }
    if (ch == '\r') { ui_submit(); return; }
    if (ch == 8) { if (in_len) in_len--; return; }
    if (ch >= 32 && ch < 127) {
        if (in_len < INPUT_MAX - 1) in_buf[in_len++] = ch;
        return;
    }
    if (ch == 0 || ch == 0xE0) {
        if (scan == 0x48 && hist_nav > 0) {              /* Up */
            hist_nav--;
            strcpy(in_buf, hist[hist_nav]);
            in_len = (int)strlen(in_buf);
        } else if (scan == 0x50) {                       /* Down */
            if (hist_nav < hist_count) hist_nav++;
            if (hist_nav == hist_count) in_len = 0;
            else {
                strcpy(in_buf, hist[hist_nav]);
                in_len = (int)strlen(in_buf);
            }
        }
    }
}

/* ---------------------------------------------------------------- main */

static void shutdown_stack(int rc)
{
    Utils::endStack();
    exit(rc);
}

int main(void)
{
    TcpSocket *listener;
    clock_t next_discovery = 0, next_spin = 0;
    int i;

    puts("DOSCHAT " DOSCHAT_VERSION
         " - combined retro agent+chat for DOS (mTCP)");

    if (Utils::parseEnv() != 0) {
        puts("mTCP environment not set up (SET MTCPCFG=..., run DHCP first)");
        return 1;
    }
    if (Utils::initStack(MAX_CLIENTS + 1, TCP_SOCKET_RING_SIZE,
                         ctrlBreakHandler, ctrlCHandler)) {
        puts("Failed to initialize TCP/IP (packet driver loaded?)");
        return 1;
    }

    memset(clients, 0, sizeof(clients));
    chatcore_init(&core, LOG_MAX_DOS);
    scratch = (char far *)_fmalloc(SCRATCH_SIZE);
    if (!scratch) {
        puts("Out of memory (response scratch buffer)");
        return 1;
    }

    listener = TcpSocketMgr::getSocket();
    listener->listen(AGENT_TCP_PORT, RECV_BUF_SIZE);

    /* paint the UI */
    {
        union REGS r;
        r.w.ax = 0x0003; int86(0x10, &r, &r);   /* clean 80x25 */
        cur_row = 0; cur_col = 0;
    }
    put_line("Retro Chat for DOS v" DOSCHAT_VERSION
             " - agent + chat in one exe", ATTR_BANNER);
    {
        char l[96];
        sprintf(l, "Agent on %d.%d.%d.%d:%d  host %s  (Esc or :quit exits)",
                MyIpAddr[0], MyIpAddr[1], MyIpAddr[2], MyIpAddr[3],
                AGENT_TCP_PORT, MyHostname);
        put_line(l, ATTR_BANNER);
    }
    put_line("----------------------------------------", ATTR_BANNER);
    draw_status_row();
    draw_input_row();

    while (!CtrlBreakDetected) {

        PACKET_PROCESS_SINGLE;
        Arp::driveArp();
        Tcp::drivePackets();

        /* accept new connections into a free slot */
        {
            TcpSocket *ns = TcpSocketMgr::accept();
            if (ns) {
                for (i = 0; i < MAX_CLIENTS; i++)
                    if (!clients[i].sock) break;
                if (i == MAX_CLIENTS) {
                    /* Tell the client why instead of a silent EOF. */
                    resp_err(ns, "DOS agent busy (no free client slot)");
                    ns->close();
                    TcpSocketMgr::freeSocket(ns);
                } else {
                    memset(&clients[i], 0, sizeof(client_t));
                    clients[i].sock = ns;
                }
            }
        }

        for (i = 0; i < MAX_CLIENTS; i++) client_pump(&clients[i]);
        service_longpolls();

        /* keyboard */
        if (biosIsKeyReady()) {
            ui_key(biosKeyRead());
            draw_input_row();
        }

        /* stream new response text into the scrollback */
        if (core.log_size != ui_log_shown) {
            ui_pump_log();
            draw_status_row();
            draw_input_row();
        }
        if (core.status_seq != ui_status_seen) {
            ui_status_seen = core.status_seq;
            draw_status_row();
            draw_input_row();
        }

        /* spinner + discovery timers */
        if (clock() >= next_spin) {
            next_spin = clock() + CLOCKS_PER_SEC / 2;
            if (ui_waiting) {
                spin_idx++;
                draw_status_row();
                draw_input_row();
            }
        }
        if (clock() >= next_discovery) {
            next_discovery = clock() + 30 * CLOCKS_PER_SEC;
            discovery_send();
        }
    }

    for (i = 0; i < MAX_CLIENTS; i++) client_drop(&clients[i]);
    listener->close();
    {
        union REGS r;
        r.w.ax = 0x0003; int86(0x10, &r, &r);
    }
    puts("DOSCHAT exiting.");
    shutdown_stack(0);
    return 0;
}
