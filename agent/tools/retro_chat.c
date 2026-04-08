/*
 * retro_chat.c - Claude Code-style chat client for Win98/XP
 *
 * Connects to the local retro_agent (127.0.0.1:9898), authenticates,
 * and presents a Claude Code-like console UI:
 *
 *   ┌──────────────────────────────────────────┐
 *   │ assistant streamed text...               │
 *   │ (scrollback history)                     │
 *   ├──────────────────────────────────────────┤
 *   │ > user input prompt                      │
 *   └──────────────────────────────────────────┘
 *
 * The bottom line is the input box. Everything above scrolls as the log
 * grows. A background polling thread fetches new log content from the
 * agent and prints it above the input area without disturbing typing.
 *
 * Build (mingw cross-compile):
 *   i686-w64-mingw32-gcc -o retro_chat.exe retro_chat.c \
 *     -lws2_32 -lkernel32 -luser32 -static -Os -s \
 *     -DWINVER=0x0410 -D_WIN32_WINNT=0x0410 \
 *     -march=i586 -mtune=pentium3
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef RETRO_CHAT_VERSION
#define RETRO_CHAT_VERSION "0.0.0"
#endif

#define AGENT_HOST   "127.0.0.1"
#define AGENT_PORT   9898
#define AGENT_SECRET "retro-agent-secret"
#define WAIT_TIMEOUT_MS 30000   /* server-side LOG_WAIT timeout */
#define SPINNER_TICK_MS  150    /* local animation tick (no network) */
#define INPUT_MAX 1024

static HANDLE g_hOut;
static HANDLE g_hIn;
static CRITICAL_SECTION g_console_cs;
static char g_input_buf[INPUT_MAX];
static int  g_input_len = 0;
static volatile int g_running = 1;
static DWORD g_log_offset = 0;

/* Subagent status — short string describing what the dev-box subagent
 * is currently doing (e.g. "EXEC dir C:\WINDOWS"). The status thread
 * polls STATUS_WAIT and updates these. The input area redraw renders
 * g_status above the spinner/prompt line when non-empty. */
#define STATUS_MAX 256
static char g_status_text[STATUS_MAX] = {0};
static volatile DWORD g_status_seq = 0;
static CRITICAL_SECTION g_status_cs;

/* ---- frame I/O ---- */

static int frame_send(SOCKET s, const char *data, DWORD len)
{
    BYTE hdr[4];
    hdr[0] = (BYTE)(len & 0xFF);
    hdr[1] = (BYTE)((len >> 8) & 0xFF);
    hdr[2] = (BYTE)((len >> 16) & 0xFF);
    hdr[3] = (BYTE)((len >> 24) & 0xFF);
    if (send(s, (const char *)hdr, 4, 0) != 4) return -1;
    if (len > 0 && send(s, data, len, 0) != (int)len) return -1;
    return 0;
}

static int frame_recv(SOCKET s, char **out_buf, DWORD *out_len)
{
    BYTE hdr[4];
    int got = 0;
    DWORD len;
    char *buf;

    while (got < 4) {
        int r = recv(s, (char *)hdr + got, 4 - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
    if (len > 4 * 1024 * 1024) return -1;

    buf = (char *)malloc(len + 1);
    if (!buf) return -1;
    got = 0;
    while ((DWORD)got < len) {
        int r = recv(s, buf + got, len - got, 0);
        if (r <= 0) { free(buf); return -1; }
        got += r;
    }
    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return 0;
}

/* ---- agent connection ---- */

static SOCKET agent_connect(void)
{
    SOCKET s;
    struct sockaddr_in addr;
    char auth[128];
    char *resp = NULL;
    DWORD resp_len = 0;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(AGENT_PORT);
    addr.sin_addr.s_addr = inet_addr(AGENT_HOST);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    /* Send AUTH */
    _snprintf(auth, sizeof(auth), "AUTH %s", AGENT_SECRET);
    if (frame_send(s, auth, (DWORD)strlen(auth)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    /* Receive auth response */
    if (frame_recv(s, &resp, &resp_len) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    /* First byte is status */
    if (resp_len < 1 || (BYTE)resp[0] == 0xFF) {
        free(resp);
        closesocket(s);
        return INVALID_SOCKET;
    }
    free(resp);
    return s;
}

static int agent_command(SOCKET s, const char *cmd, char **out_text, DWORD *out_len)
{
    char *resp = NULL;
    DWORD resp_len = 0;

    if (frame_send(s, cmd, (DWORD)strlen(cmd)) != 0) return -1;
    if (frame_recv(s, &resp, &resp_len) != 0) return -1;

    if (resp_len < 1) { free(resp); return -1; }
    if ((BYTE)resp[0] == 0xFF) {
        /* error */
        free(resp);
        return -1;
    }
    /* Skip status byte */
    if (out_text) {
        DWORD textlen = resp_len - 1;
        char *text = (char *)malloc(textlen + 1);
        if (text) {
            memcpy(text, resp + 1, textlen);
            text[textlen] = '\0';
            *out_text = text;
            if (out_len) *out_len = textlen;
        }
    }
    free(resp);
    return 0;
}

/* ---- console UI (Claude Code-style) ----
 *
 * UX model (matches Claude Code):
 *
 *   ... (scrollback) ...
 *   > hello world          <-- committed user prompt (magenta '>' + default text)
 *   Hello! Here's the...   <-- assistant response in default color
 *   > _                    <-- new input line (magenta '>' + cursor)
 *
 * While waiting for the first response chunk after Enter, the input area
 * grows by one line to show a spinner status:
 *
 *   > what's the time?
 *   * Working... |         <-- spinner line in yellow, animates ~5x/sec
 *   > _                    <-- input line stays at the bottom
 *
 * As soon as response content arrives, the spinner is removed and the
 * content takes its place. Subsequent chunks append below.
 *
 * Color scheme (chosen to mirror Claude Code's accent palette):
 *   DEFAULT  - bright white  - typed input text and assistant responses
 *   PROMPT   - bright magenta - the '>' prompt prefix
 *   SPINNER  - bright yellow - the "* Working... X" status line
 *   BANNER   - gray          - startup banner
 *
 * On Enter, the chat client displays the prompt LOCALLY (instant feedback)
 * and sends only PROMPT_PUSH to the proxy. The proxy never echoes the
 * prompt back — it only streams the response. This avoids the round-trip
 * delay between hitting Enter and seeing your text in the scrollback.
 */

#define COLOR_DEFAULT   (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)  /* bright white — user input */
#define COLOR_RESPONSE  (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)                  /* bright cyan — assistant response */
#define COLOR_PROMPT    (FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY)                    /* bright magenta — '>' prefix */
#define COLOR_SPINNER   (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)                   /* bright yellow — spinner */
#define COLOR_STATUS    (FOREGROUND_GREEN | FOREGROUND_INTENSITY)                                    /* bright green — subagent status */
#define COLOR_BANNER    (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)                        /* gray — banner */

static int g_screen_w = 80;
static int g_screen_h = 25;
static volatile int g_waiting = 0;     /* 1 = prompt sent, awaiting first chunk (spinner shows) */
static int g_spinner_idx = 0;
static const char SPINNER_CHARS[] = "|/-\\";

static void set_color(WORD attrs)
{
    SetConsoleTextAttribute(g_hOut, attrs);
}

static void get_console_size(void)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_hOut, &csbi)) {
        g_screen_w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        g_screen_h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
}

/* The "input area" at the bottom of the buffer is either 1 line (just the
 * input prompt) or 2 lines (spinner status + input prompt) when waiting
 * for the first response chunk. We track its current height so we know
 * how many rows to erase when redrawing or when scrolling new content. */
static int g_input_area_height = 1;

/* Erase N lines starting at the current cursor row going upward. Cursor
 * ends up at column 0 of the topmost erased row. */
static void erase_input_area(void)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD written;
    COORD pos;
    int row;
    int top;

    if (!GetConsoleScreenBufferInfo(g_hOut, &csbi)) return;

    top = csbi.dwCursorPosition.Y - (g_input_area_height - 1);
    if (top < 0) top = 0;

    for (row = top; row <= csbi.dwCursorPosition.Y; row++) {
        pos.X = 0;
        pos.Y = (SHORT)row;
        FillConsoleOutputCharacterA(g_hOut, ' ', g_screen_w, pos, &written);
    }
    pos.X = 0;
    pos.Y = (SHORT)top;
    SetConsoleCursorPosition(g_hOut, pos);
}

/* Draw the input area at the current cursor position.
 *
 * Layout (top to bottom):
 *   - Optional status line:  "[subagent: doing X]"  (only if g_status_text non-empty)
 *   - Optional spinner line: "* Working... X"        (only if g_waiting set)
 *   - Input prompt line:     "> <text>"
 *
 * g_input_area_height is set to the number of lines drawn so future
 * erase_input_area() calls know how much to wipe.
 *
 * Cursor is left at the end of the input text (the typing position).
 */
static void draw_input_area(void)
{
    DWORD written;
    char buf[STATUS_MAX + 64];
    int max_show;
    int start = 0;
    int height = 1;
    int has_status = 0;
    char status_local[STATUS_MAX];

    /* Snapshot status under its own lock — avoid holding console_cs
     * while touching status_cs. */
    EnterCriticalSection(&g_status_cs);
    if (g_status_text[0]) {
        strncpy(status_local, g_status_text, sizeof(status_local) - 1);
        status_local[sizeof(status_local) - 1] = '\0';
        has_status = 1;
    } else {
        status_local[0] = '\0';
    }
    LeaveCriticalSection(&g_status_cs);

    if (has_status) {
        int max_status_show = g_screen_w - 12;
        if (max_status_show < 10) max_status_show = 10;
        if ((int)strlen(status_local) > max_status_show) {
            status_local[max_status_show - 3] = '.';
            status_local[max_status_show - 2] = '.';
            status_local[max_status_show - 1] = '.';
            status_local[max_status_show] = '\0';
        }
        set_color(COLOR_STATUS);
        _snprintf(buf, sizeof(buf), "[subagent: %s]\n", status_local);
        WriteConsoleA(g_hOut, buf, (DWORD)strlen(buf), &written, NULL);
        height++;
    }

    if (g_waiting) {
        set_color(COLOR_SPINNER);
        _snprintf(buf, sizeof(buf),
                  "* Working... %c\n",
                  SPINNER_CHARS[g_spinner_idx & 3]);
        WriteConsoleA(g_hOut, buf, (DWORD)strlen(buf), &written, NULL);
        height++;
    }

    g_input_area_height = height;

    set_color(COLOR_PROMPT);
    WriteConsoleA(g_hOut, "> ", 2, &written, NULL);

    set_color(COLOR_DEFAULT);
    max_show = g_screen_w - 3;
    if (g_input_len > max_show) start = g_input_len - max_show;
    if (g_input_len > 0) {
        WriteConsoleA(g_hOut, g_input_buf + start,
                      g_input_len - start, &written, NULL);
    }
}

/* Erase + redraw the input area (called on input change or spinner tick) */
static void refresh_input(void)
{
    EnterCriticalSection(&g_console_cs);
    erase_input_area();
    draw_input_area();
    LeaveCriticalSection(&g_console_cs);
}

/* Sanitize a chunk of log content into a print-safe buffer.
 *
 * Strips:
 *   - \x01 (USER_MARKER from old proxy versions — defensive)
 *   - other ASCII control bytes except \n, \r, \t
 *   - high-bit bytes that the Win98 console may render as garbage
 *
 * Returns the number of bytes written to `out` (always <= len).
 */
static DWORD sanitize_chunk(const char *in, DWORD len, char *out)
{
    DWORD i, j = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            out[j++] = (char)c;
        } else if (c < 32) {
            /* skip other control bytes (incl. \x01 marker) */
            continue;
        } else if (c < 127) {
            out[j++] = (char)c;
        } else {
            /* skip high-bit bytes — would render as garbage on Win98 console */
            continue;
        }
    }
    return j;
}

/* Word-wrap text to a maximum column width.
 *
 * Wraps at word boundaries (spaces). Existing newlines are preserved.
 * Words longer than the max are hard-wrapped (split mid-word).
 *
 * Assumes the cursor is at column 0 when this output starts. The output
 * buffer must be at least 2 * len + 16 bytes to accommodate inserted
 * newlines.
 *
 * Returns the number of bytes written to `out`.
 */
static DWORD wrap_text(const char *in, DWORD len, char *out, int max_col)
{
    DWORD i = 0, j = 0;
    int col = 0;

    if (max_col < 10) max_col = 10;

    while (i < len) {
        unsigned char c = (unsigned char)in[i];

        if (c == '\n') {
            out[j++] = '\n';
            col = 0;
            i++;
            continue;
        }
        if (c == '\r') {
            i++;
            continue;
        }
        if (c == ' ') {
            /* Standalone space — only emit if not at start of line */
            if (col > 0 && col < max_col) {
                out[j++] = ' ';
                col++;
            }
            i++;
            continue;
        }

        /* Find the end of the current word */
        DWORD word_start = i;
        while (i < len) {
            unsigned char w = (unsigned char)in[i];
            if (w == ' ' || w == '\n' || w == '\r') break;
            i++;
        }
        DWORD word_len = i - word_start;
        if (word_len == 0) continue;

        /* If the word doesn't fit on this line, wrap first */
        if (col + (int)word_len > max_col && col > 0) {
            out[j++] = '\n';
            col = 0;
        }

        /* Emit the word — hard-wrap if longer than the whole line */
        if ((int)word_len > max_col) {
            DWORD k;
            for (k = 0; k < word_len; k++) {
                if (col >= max_col) {
                    out[j++] = '\n';
                    col = 0;
                }
                out[j++] = in[word_start + k];
                col++;
            }
        } else {
            DWORD k;
            for (k = 0; k < word_len; k++) {
                out[j++] = in[word_start + k];
            }
            col += word_len;
        }
    }

    return j;
}

/* Print log content (a chunk of assistant response) above the input area.
 *
 * Pattern:
 *   1. Erase the current input area (1 or 2 lines)
 *   2. Sanitize the chunk (strip control bytes / high bytes)
 *   3. Write the response text in COLOR_RESPONSE (bright cyan)
 *   4. End on a newline so the input line starts fresh
 *   5. Clear g_waiting (response is now flowing — spinner gone)
 *   6. Redraw input area below (1 line, no spinner)
 */
static void print_log_chunk(const char *text, DWORD len)
{
    DWORD written;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    char *clean;
    char *wrapped;
    DWORD clean_len;
    DWORD wrapped_len;
    int max_col;

    if (len == 0) return;

    /* Refresh screen width for wrapping (in case the user resized) */
    if (GetConsoleScreenBufferInfo(g_hOut, &csbi)) {
        g_screen_w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        g_screen_h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
    /* Wrap one column shy of the right edge to avoid auto-wrap glitches */
    max_col = g_screen_w - 1;
    if (max_col < 20) max_col = 20;

    /* Sanitize into a temp buffer */
    clean = (char *)malloc(len + 2);
    if (!clean) return;
    clean_len = sanitize_chunk(text, len, clean);
    if (clean_len == 0) { free(clean); return; }

    /* Word-wrap into another buffer (worst case ~2x size for inserted \n) */
    wrapped = (char *)malloc(clean_len * 2 + 16);
    if (!wrapped) { free(clean); return; }
    wrapped_len = wrap_text(clean, clean_len, wrapped, max_col);

    EnterCriticalSection(&g_console_cs);

    erase_input_area();

    /* Write the response chunk in cyan (distinct from input white) */
    set_color(COLOR_RESPONSE);
    WriteConsoleA(g_hOut, wrapped, wrapped_len, &written, NULL);

    /* Ensure we end on a newline */
    if (wrapped_len == 0 || wrapped[wrapped_len - 1] != '\n') {
        WriteConsoleA(g_hOut, "\n", 1, &written, NULL);
    }

    /* Stop waiting — content is flowing */
    g_waiting = 0;

    /* Redraw input area (now 1 line, no spinner) */
    draw_input_area();

    LeaveCriticalSection(&g_console_cs);
    free(clean);
    free(wrapped);
}

/* ---- polling thread ---- */

/* Long-polling thread: blocks on LOG_WAIT until new content arrives or
 * the server-side timeout expires (30s). Re-issues immediately. Zero
 * polling traffic — only sends a request when the previous one returns.
 *
 * Sub-100ms latency for new content; idle CPU/network is essentially
 * zero (the socket is parked in recv() kernel-side).
 */
static DWORD WINAPI wait_thread(LPVOID param)
{
    SOCKET s = (SOCKET)param;
    char cmd[64];

    while (g_running) {
        char *resp = NULL;
        DWORD resp_len = 0;

        _snprintf(cmd, sizeof(cmd), "LOG_WAIT %lu %d",
                  (unsigned long)g_log_offset, WAIT_TIMEOUT_MS);
        if (agent_command(s, cmd, &resp, &resp_len) == 0 && resp) {
            /* Format: "<total_size>\n<bytes>" */
            char *nl = strchr(resp, '\n');
            if (nl) {
                DWORD total_size = (DWORD)strtoul(resp, NULL, 10);
                const char *body = nl + 1;
                DWORD body_len = resp_len - (DWORD)(body - resp);
                if (body_len > 0) {
                    print_log_chunk(body, body_len);
                    g_log_offset += body_len;
                }
                if (total_size < g_log_offset) {
                    /* Log was cleared/reset on the agent */
                    g_log_offset = 0;
                }
            }
            free(resp);
        } else {
            /* Connection lost — small backoff before retrying */
            Sleep(1000);
        }
    }
    return 0;
}

/* Status thread: long-polls STATUS_WAIT on a dedicated connection. The
 * subagent calls STATUS_SET on the agent before each tool use to report
 * what it's about to do. We display this above the spinner so the user
 * knows what's happening even when no log content has streamed yet.
 *
 * Format from STATUS_WAIT: "<seq>\n<status_text>". On change, copy
 * status_text into g_status_text and refresh the input area.
 */
static DWORD WINAPI status_thread(LPVOID param)
{
    SOCKET s = (SOCKET)param;
    char cmd[64];

    while (g_running) {
        char *resp = NULL;
        DWORD resp_len = 0;
        DWORD known = g_status_seq;

        _snprintf(cmd, sizeof(cmd), "STATUS_WAIT %lu %d",
                  (unsigned long)known, WAIT_TIMEOUT_MS);
        if (agent_command(s, cmd, &resp, &resp_len) == 0 && resp) {
            char *nl = strchr(resp, '\n');
            if (nl) {
                DWORD new_seq = (DWORD)strtoul(resp, NULL, 10);
                const char *body = nl + 1;
                if (new_seq != known) {
                    EnterCriticalSection(&g_status_cs);
                    strncpy(g_status_text, body,
                            sizeof(g_status_text) - 1);
                    g_status_text[sizeof(g_status_text) - 1] = '\0';
                    g_status_seq = new_seq;
                    LeaveCriticalSection(&g_status_cs);
                    refresh_input();
                }
            }
            free(resp);
        } else {
            Sleep(1000);
        }
    }
    return 0;
}

/* Spinner thread: animates the "* Working... X" status line every
 * SPINNER_TICK_MS while g_waiting is set. No network traffic — purely
 * local console updates. Sleeps when not waiting.
 */
static DWORD WINAPI spinner_thread(LPVOID param)
{
    (void)param;
    while (g_running) {
        if (g_waiting) {
            g_spinner_idx = (g_spinner_idx + 1) & 3;
            refresh_input();
            Sleep(SPINNER_TICK_MS);
        } else {
            Sleep(100);  /* light idle */
        }
    }
    return 0;
}

/* ---- main ---- */

static void print_banner(void)
{
    DWORD written;
    char banner[512];
    set_color(COLOR_BANNER);
    _snprintf(banner, sizeof(banner),
        "Retro Chat v%s - Claude Code-style interface\n"
        "Connected to local retro agent. Type a prompt and press Enter.\n"
        "Type :quit to exit, :clear to clear the log.\n"
        "----------------------------------------\n",
        RETRO_CHAT_VERSION);
    WriteConsoleA(g_hOut, banner, (DWORD)strlen(banner), &written, NULL);
    set_color(COLOR_DEFAULT);
}

int main(void)
{
    WSADATA wsa;
    SOCKET s;
    HANDLE wait_h, spin_h;
    DWORD tid;
    INPUT_RECORD ir;
    DWORD nread;

    if (WSAStartup(MAKEWORD(2, 0), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hIn = GetStdHandle(STD_INPUT_HANDLE);
    InitializeCriticalSection(&g_console_cs);
    InitializeCriticalSection(&g_status_cs);
    get_console_size();

    /* Set raw input mode */
    SetConsoleMode(g_hIn, ENABLE_WINDOW_INPUT);

    s = agent_connect();
    if (s == INVALID_SOCKET) {
        fprintf(stderr, "Cannot connect to retro agent at %s:%d\n",
                AGENT_HOST, AGENT_PORT);
        WSACleanup();
        return 1;
    }

    /* Fresh session: clear any previous chat history from the agent's
     * log buffer. Resume functionality can be added later. */
    agent_command(s, "LOG_CLEAR", NULL, NULL);
    g_log_offset = 0;

    print_banner();
    /* Banner ends with newline; just draw the input area below it */
    EnterCriticalSection(&g_console_cs);
    draw_input_area();
    LeaveCriticalSection(&g_console_cs);

    /* Start three background threads, each on its own dedicated socket
     * (or none for the spinner):
     *   - wait_thread:    long-polls LOG_WAIT for response chunks
     *   - status_thread:  long-polls STATUS_WAIT for subagent status
     *   - spinner_thread: animates the spinner locally (no socket)
     */
    {
        SOCKET wait_sock = agent_connect();
        SOCKET status_sock;
        HANDLE status_h;
        if (wait_sock == INVALID_SOCKET) {
            fprintf(stderr, "Failed to open wait connection\n");
            closesocket(s);
            WSACleanup();
            return 1;
        }
        status_sock = agent_connect();
        if (status_sock == INVALID_SOCKET) {
            fprintf(stderr, "Failed to open status connection\n");
            closesocket(wait_sock);
            closesocket(s);
            WSACleanup();
            return 1;
        }
        wait_h = CreateThread(NULL, 0, wait_thread, (LPVOID)wait_sock, 0, &tid);
        status_h = CreateThread(NULL, 0, status_thread, (LPVOID)status_sock, 0, &tid);
        spin_h = CreateThread(NULL, 0, spinner_thread, NULL, 0, &tid);
        /* status_h handle is leaked deliberately — it's a daemon thread.
         * On exit we just close the underlying socket via WSACleanup. */
        (void)status_h;
    }

    /* Input loop */
    while (g_running) {
        if (!ReadConsoleInputA(g_hIn, &ir, 1, &nread)) break;
        if (ir.EventType != KEY_EVENT) continue;
        if (!ir.Event.KeyEvent.bKeyDown) continue;

        {
            WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
            CHAR ch = ir.Event.KeyEvent.uChar.AsciiChar;

            if (vk == VK_RETURN) {
                if (g_input_len > 0) {
                    char cmd[INPUT_MAX + 64];
                    g_input_buf[g_input_len] = '\0';

                    /* Local commands */
                    if (strcmp(g_input_buf, ":quit") == 0) {
                        g_running = 0;
                        break;
                    }
                    if (strcmp(g_input_buf, ":clear") == 0) {
                        agent_command(s, "LOG_CLEAR", NULL, NULL);
                        g_log_offset = 0;
                        EnterCriticalSection(&g_console_cs);
                        {
                            DWORD w;
                            COORD home;
                            home.X = 0; home.Y = 0;
                            FillConsoleOutputCharacterA(g_hOut, ' ',
                                g_screen_w * g_screen_h, home, &w);
                            SetConsoleCursorPosition(g_hOut, home);
                        }
                        print_banner();
                        draw_input_area();
                        LeaveCriticalSection(&g_console_cs);
                        g_input_len = 0;
                        continue;
                    }

                    /* Send the prompt to the proxy. The proxy is the
                     * single source of truth for displaying the prompt.
                     * The proxy does NOT echo it back. We "commit" the
                     * current input line to scrollback by writing a
                     * newline (the typed text stays on-screen), then
                     * transition to the waiting state which redraws the
                     * input area with the spinner above a new empty
                     * input line. */
                    {
                        char saved_prompt[INPUT_MAX];
                        memcpy(saved_prompt, g_input_buf, g_input_len + 1);

                        EnterCriticalSection(&g_console_cs);
                        {
                            DWORD w;
                            g_input_area_height = 1;
                            WriteConsoleA(g_hOut, "\n", 1, &w, NULL);
                            g_input_len = 0;
                            g_waiting = 1;
                            g_spinner_idx = 0;
                            draw_input_area();
                        }
                        LeaveCriticalSection(&g_console_cs);

                        /* Send the prompt to the proxy via the local agent */
                        _snprintf(cmd, sizeof(cmd), "PROMPT_PUSH %s",
                                  saved_prompt);
                        agent_command(s, cmd, NULL, NULL);
                    }
                }
            } else if (vk == VK_BACK) {
                if (g_input_len > 0) {
                    g_input_len--;
                    refresh_input();
                }
            } else if (ch >= 32 && ch < 127) {
                if (g_input_len < INPUT_MAX - 1) {
                    g_input_buf[g_input_len++] = ch;
                    refresh_input();
                }
            }
        }
    }

    g_running = 0;
    WaitForSingleObject(wait_h, 2000);
    WaitForSingleObject(spin_h, 500);
    CloseHandle(wait_h);
    CloseHandle(spin_h);
    closesocket(s);
    WSACleanup();
    return 0;
}
