/*
 * retro_agent - main.c
 * Entry point for the retro PC remote management agent.
 * Runs a TCP server on port 9898 and UDP discovery broadcaster on port 9899.
 * Compatible with Win98SE and WinXP.
 *
 * Supports three client handling modes:
 *   Multiplex (default) - select()-based, up to MAX_CLIENTS concurrent connections
 *   Single   (-1 flag)  - one client at a time, inline handling (legacy)
 *   Threaded (-t flag)   - thread per client (requires NT Winsock)
 *
 * Supports two run modes:
 *   Console mode - started manually or via Run key (default)
 *   Service mode - started by NT Service Control Manager (XP/2000)
 *
 * ---------------------------------------------------------------------------
 * WHAT HAS CRASHED / HUNG THIS AGENT ON OLD HARDWARE (and how it's handled now
 * — all fixed as of the current build; do NOT regress these):
 *
 * Genuine Pentium-1 / Windows 98 box (Compaq Deskpro 2000). The rest of the
 * fleet is XP on i686+ CPUs, so none of these bit until a real P5/Win98 box
 * joined:
 *
 *  1. try_service_start() called StartServiceCtrlDispatcherA without an
 *     is_nt() guard -> silent exit before any logging on Win9x. FIX: gate on
 *     is_nt() in service.c (never attempt SCM dispatch on 9x).
 *  2. SetHandleInformation is a Windows-2000+ import -> the Win98 loader
 *     couldn't resolve it and refused to load the EXE (silent, pre-main).
 *     FIX: resolve it dynamically (util.c set_handle_noinherit); no-op on 9x.
 *  3. CMOV (i686) instructions from the MinGW runtime -> STATUS_ILLEGAL_
 *     INSTRUCTION (0xc000001d) on the classic Pentium (P5, no CMOV). Two
 *     sources: MinGW ANSI stdio (fixed via -D__USE_MINGW_ANSI_STDIO=0 ->
 *     msvcrt) and __thread emulated-TLS (fixed by using native Win32 TLS
 *     here instead of __thread). Our own code is -march=i586 (CMOV-free).
 *  4. Not a crash but a hang: auto-onboarding at startup saturated the P1 for
 *     minutes (SMB wallpaper/game copy) and made the agent look dead. FIX:
 *     it was moved off the boot path, and in v1.71.0 removed outright -
 *     GAMESYNC does the same work from the staged library, on its own
 *     schedule, and now gates each title on what the machine can run.
 *  5. Silent failures were invisible because logging was stderr-only and via
 *     msvcrt stdio (which can fault on 9x). FIX: default-on RAW-Win32 rotating
 *     file log (log.c) with startup breadcrumbs + a lock-free crash logger, so
 *     any future crash leaves an on-disk trail (and is mirrored to the share).
 *
 *  6. CreateThread with lpThreadId = NULL. Legal on NT, REJECTED on Win95/98
 *     with ERROR_INVALID_PARAMETER (87). Every fire-and-forget helper passed
 *     NULL, so on the Win98 box automap, autoupdate, retrowall, watchdog,
 *     ai_status, sharelog and dosstage ALL silently never started - for
 *     years. Only dosstage checked its return value, and its message blamed
 *     memory, so the real cause hid behind a wrong guess on a box that had
 *     87MB free. That is why auto-update never worked there, why the log was
 *     never mirrored to the share, and why the share needed mapping by hand.
 *     FIX: spawn_helper() passes &tid, checks the result, and names the
 *     actual error. Do not pass NULL.
 *
 * Also load-bearing on Win9x: __thread -> native TLS (above); Toolhelp32 is
 * fine on 9x (it originated there) but is NOT on NT4 — a non-issue for the
 * 98/XP fleet. Keep new startup work off the hot path and OS-gated.
 * ---------------------------------------------------------------------------
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

#include "protocol.h"
#include "handlers.h"
#include "util.h"
#include "log.h"

#ifndef AGENT_VERSION
#define AGENT_VERSION "0.0.0"
#endif

/* Default shared secret - override via command line */
char g_secret[256] = "retro-agent-secret";
char g_logfile[256] = "";

/* Client handling mode */
#define MODE_MULTIPLEX  0   /* -m: select()-based multi-client (serialized) */
#define MODE_SINGLE     1   /* -1: old single-client inline */
#define MODE_THREADED   2   /* default: thread per client (truly concurrent) */
static int g_client_mode = MODE_THREADED;

/* Multiplexed client slots */
/* Concurrent client slots. Four was not enough for a box that runs the chat
 * client locally: retro_chat holds THREE connections of its own (command,
 * log long-poll, status long-poll) and the fleet daemon needs TWO more
 * (wait + send) — so the daemon could not attach, nobody polled that box's
 * prompts, and typing into its chat produced no reply at all. Any further
 * client (an operator, a skill) then got "max connections reached".
 * A slot is a socket handle, a flag and a 24-byte string, so the cost of
 * headroom is trivial even on the 31MB Win98 box. */
#define MAX_CLIENTS 10

typedef struct {
    SOCKET sock;
    int    authed;
    DWORD  last_active;   /* GetTickCount of the last byte from this client */
    char   addr_str[24];  /* "x.x.x.x:port" for logging */
} client_slot_t;

/*
 * Drop a connection that has gone quiet for this long.
 *
 * Slots were held until the peer disconnected, which a half-open TCP
 * connection never does - a client whose machine went away, or a probe that
 * connected and vanished, kept its slot forever. Ten of those and the agent
 * is unreachable while looking perfectly healthy.
 *
 * Comfortably longer than any legitimate quiet period: the longest thing a
 * client does without speaking is a LOG_WAIT/STATUS_WAIT long-poll, capped at
 * 30s (1s on Win9x), so anything silent for five minutes is gone.
 */
#define CLIENT_IDLE_MS  300000

/* Held for the process lifetime so a second copy of the agent refuses to
 * start rather than racing this one for the ports. */
static HANDLE g_instance_mutex = NULL;
#define AGENT_INSTANCE_MUTEX "RetroAgentSingleInstance"

static client_slot_t g_clients[MAX_CLIENTS];

/* Cached system info for discovery packets */
static char g_hostname[256]  = "";
static char g_local_ip[64]   = "";
static char g_os_str[64]     = "";
static char g_cpu_str[128]   = "";
static DWORD g_ram_mb        = 0;

volatile int g_running = 1;
int g_longpoll_max_ms = 0;   /* set below once the client mode is known */

/* Exception recovery for command handlers — per-thread state via NATIVE
 * Windows TLS. Deliberately NOT __thread: MinGW compiles __thread to EMULATED
 * TLS (___emutls_get_address), which contains a CMOV instruction — illegal on
 * a genuine Pentium 1 (STATUS_ILLEGAL_INSTRUCTION 0xc000001d). TlsAlloc/
 * TlsGetValue/TlsSetValue exist on Win95+ and are CMOV-free. Each client
 * thread gets its own jmp_buf so concurrent commands (NT threaded mode) don't
 * corrupt each other. */
typedef struct {
    jmp_buf handler_jmp;
    int     in_handler;
    DWORD   exception_code;
} handler_state_t;

static DWORD g_hs_tls = TLS_OUT_OF_INDEXES;

/* Return this thread's handler state, lazily allocating it on first use.
 * Returns NULL only if TLS wasn't initialized or the allocation failed. */
static handler_state_t *handler_state(void)
{
    handler_state_t *hs;
    if (g_hs_tls == TLS_OUT_OF_INDEXES)
        return NULL;
    hs = (handler_state_t *)TlsGetValue(g_hs_tls);
    if (!hs) {
        hs = (handler_state_t *)HeapAlloc(GetProcessHeap(),
                                          HEAP_ZERO_MEMORY, sizeof(*hs));
        if (hs)
            TlsSetValue(g_hs_tls, hs);
    }
    return hs;
}

static LONG WINAPI command_exception_filter(PEXCEPTION_POINTERS info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    void *addr = (void *)info->ExceptionRecord->ExceptionAddress;
    handler_state_t *hs = handler_state();

    /* Record the fault straight to disk (lock-free) — the whole point of the
     * crash logger: a fault in a startup/background thread (not inside a
     * command handler, so we can't longjmp out of it) would otherwise take
     * the process down leaving no trace. On NT this filter is reliably
     * invoked; on Win9x it may not be, which is why main()/agent_run() also
     * emit dense breadcrumbs so the last logged line locates the crash. */
    log_crash(LOG_MAIN, "*** UNHANDLED EXCEPTION code=0x%08lx addr=%p "
              "in_handler=%d ***", (unsigned long)code, addr,
              hs ? hs->in_handler : -1);

    if (hs && hs->in_handler) {
        hs->exception_code = code;
        longjmp(hs->handler_jmp, 1);
    }
    log_crash(LOG_MAIN, "*** fatal exception outside a handler; process "
              "terminating ***");
    return EXCEPTION_CONTINUE_SEARCH;
}

static void get_local_ip(char *buf, int bufsize)
{
    char name[256];
    struct hostent *he;

    if (gethostname(name, sizeof(name)) == 0) {
        he = gethostbyname(name);
        if (he && he->h_addr_list[0]) {
            struct in_addr addr;
            memcpy(&addr, he->h_addr_list[0], sizeof(addr));
            safe_strncpy(buf, inet_ntoa(addr), bufsize);
            return;
        }
    }
    safe_strncpy(buf, "0.0.0.0", bufsize);
}

/*
 * Add Windows Firewall exception for the agent (XP SP2+).
 * Uses "netsh firewall" which exists on XP SP2+ but not Win98SE.
 * Silently fails on Win9x where there's no firewall to worry about.
 */
static void ensure_firewall_exception(void)
{
    char exe_path[MAX_PATH];
    char cmd[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

    _snprintf(cmd, sizeof(cmd),
        "netsh firewall add allowedprogram \"%s\" \"Retro Agent\" ENABLE",
        exe_path);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        log_msg(LOG_MAIN, "Firewall exception added (netsh firewall)");
    }

    /* Also try the newer "netsh advfirewall" syntax (Vista+ / some XP SP3) */
    _snprintf(cmd, sizeof(cmd),
        "netsh advfirewall firewall add rule name=\"Retro Agent\" "
        "dir=in action=allow program=\"%s\" enable=yes protocol=tcp localport=%d",
        exe_path, AGENT_TCP_PORT);

    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        log_msg(LOG_MAIN, "Firewall exception added (netsh advfirewall)");
    }
}

static void cache_system_info(void)
{
    SYSTEM_INFO si;
    OSVERSIONINFOA osvi;
    MEMORYSTATUS ms;

    {
        DWORD hn_size = sizeof(g_hostname);
        GetComputerNameA(g_hostname, &hn_size);
    }
    get_local_ip(g_local_ip, sizeof(g_local_ip));

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    _snprintf(g_os_str, sizeof(g_os_str), "Win%lu.%lu.%lu",
              osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);

    GetSystemInfo(&si);
    _snprintf(g_cpu_str, sizeof(g_cpu_str), "x86_%lu_cores",
              (unsigned long)si.dwNumberOfProcessors);

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    g_ram_mb = (DWORD)(ms.dwTotalPhys / (1024 * 1024));
}

/*
 * Compute the subnet-directed broadcast address from local IP.
 * Win98 Winsock often ignores 255.255.255.255 (INADDR_BROADCAST)
 * but works with the subnet broadcast (e.g. 192.168.1.255).
 * Falls back to 255.255.255.255 if detection fails.
 */
static unsigned long get_subnet_broadcast(void)
{
    unsigned long ip_addr, subnet_bcast;
    DWORD dwBytesReturned = 0;
    SOCKET tmp;

    /* Try gethostbyname approach first - most compatible */
    ip_addr = inet_addr(g_local_ip);
    if (ip_addr == INADDR_NONE || ip_addr == 0)
        return INADDR_BROADCAST;

    /* Assume /24 subnet (most common home network) as safe default */
    subnet_bcast = (ip_addr & htonl(0xFFFFFF00)) | htonl(0x000000FF);

    /* Try to get actual subnet mask via WSAIoctl SIO_GET_INTERFACE_LIST.
     * This may not be available on Win98 - fall back to /24 assumption. */
    tmp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tmp != INVALID_SOCKET) {
        /* SIO_GET_INTERFACE_LIST = 0x4004747F */
        char ifbuf[2048];
        if (WSAIoctl(tmp, 0x4004747F, NULL, 0, ifbuf, sizeof(ifbuf),
                     &dwBytesReturned, NULL, NULL) == 0) {
            /* Each entry is 76 bytes: sockaddr_in[3] + flags(DWORD) */
            unsigned int i;
            unsigned int num_if = dwBytesReturned / 76;
            for (i = 0; i < num_if; i++) {
                struct sockaddr_in *addr = (struct sockaddr_in *)(ifbuf + i * 76);
                struct sockaddr_in *mask = (struct sockaddr_in *)(ifbuf + i * 76 + 32);
                if (addr->sin_addr.s_addr == ip_addr) {
                    subnet_bcast = (ip_addr & mask->sin_addr.s_addr) |
                                   ~mask->sin_addr.s_addr;
                    break;
                }
            }
        }
        closesocket(tmp);
    }

    return subnet_bcast;
}

/*
 * UDP Discovery broadcaster thread.
 * Sends discovery packet every DISCOVERY_INTERVAL ms.
 * Also listens for DISCOVER probes and responds immediately.
 * Uses subnet-directed broadcast for Win98 compatibility.
 */
static DWORD WINAPI discovery_thread(LPVOID param)
{
    SOCKET udp_sock;
    struct sockaddr_in bcast_addr, subnet_bcast_addr, bind_addr, from_addr;
    int from_len;
    char packet[512];
    BOOL bcast_enable = TRUE;
    fd_set readfds;
    struct timeval tv;
    unsigned long subnet_bcast;

    (void)param;
    log_msg(LOG_NET, "discovery thread started");

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock == INVALID_SOCKET) {
        log_msg(LOG_MAIN, "discovery: socket() failed: %d", WSAGetLastError());
        return 1;
    }

    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST,
               (const char *)&bcast_enable, sizeof(bcast_enable));

    /* Also allow reuse so we can bind and broadcast on same port */
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&bcast_enable, sizeof(bcast_enable));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(AGENT_UDP_PORT);
    if (bind(udp_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
        log_msg(LOG_MAIN, "discovery: bind() failed: %d", WSAGetLastError());
    }

    /* 255.255.255.255 broadcast */
    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
    bcast_addr.sin_port = htons(AGENT_UDP_PORT);

    /* Subnet-directed broadcast (e.g. 192.168.1.255) */
    subnet_bcast = get_subnet_broadcast();
    memset(&subnet_bcast_addr, 0, sizeof(subnet_bcast_addr));
    subnet_bcast_addr.sin_family = AF_INET;
    subnet_bcast_addr.sin_addr.s_addr = subnet_bcast;
    subnet_bcast_addr.sin_port = htons(AGENT_UDP_PORT);

    {
        struct in_addr sb;
        sb.s_addr = subnet_bcast;
        log_msg(LOG_MAIN, "discovery: subnet broadcast = %s", inet_ntoa(sb));
    }

    discovery_build_packet(packet, sizeof(packet), g_hostname, g_local_ip,
                           AGENT_TCP_PORT, g_os_str, g_cpu_str, g_ram_mb);

    while (g_running) {
        int rc;

        /* Send to subnet broadcast (works on Win98) */
        rc = sendto(udp_sock, packet, (int)strlen(packet), 0,
                    (struct sockaddr *)&subnet_bcast_addr, sizeof(subnet_bcast_addr));
        if (rc == SOCKET_ERROR) {
            log_msg(LOG_MAIN, "discovery: subnet sendto failed: %d", WSAGetLastError());
        }

        /* Also send to 255.255.255.255 (works on XP+) */
        sendto(udp_sock, packet, (int)strlen(packet), 0,
               (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));

        /* Wait for DISCOVER probes or timeout */
        {
            DWORD elapsed = 0;
            while (elapsed < DISCOVERY_INTERVAL && g_running) {
                FD_ZERO(&readfds);
                FD_SET(udp_sock, &readfds);
                tv.tv_sec = 1;
                tv.tv_usec = 0;

                if (select(0, &readfds, NULL, NULL, &tv) > 0) {
                    char probe[64];
                    from_len = sizeof(from_addr);
                    int n = recvfrom(udp_sock, probe, sizeof(probe) - 1, 0,
                                     (struct sockaddr *)&from_addr, &from_len);
                    if (n > 0) {
                        probe[n] = '\0';
                        if (strcmp(probe, "DISCOVER") == 0) {
                            /* Respond directly to the requester */
                            sendto(udp_sock, packet, (int)strlen(packet), 0,
                                   (struct sockaddr *)&from_addr, from_len);
                        }
                    }
                }
                elapsed += 1000;
            }
        }
    }

    closesocket(udp_sock);
    return 0;
}

/* ---- Multiplexed client management ---- */

static void clients_init(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].sock = INVALID_SOCKET;
        g_clients[i].authed = 0;
        g_clients[i].addr_str[0] = '\0';
    }
}

static int clients_find_free(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].sock == INVALID_SOCKET)
            return i;
    return -1;
}

static int clients_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].sock != INVALID_SOCKET)
            n++;
    return n;
}

static void client_drop(int slot)
{
    if (g_clients[slot].sock != INVALID_SOCKET) {
        log_msg(LOG_MAIN, "Client %d (%s) disconnected",
                slot, g_clients[slot].addr_str);
        closesocket(g_clients[slot].sock);
        g_clients[slot].sock = INVALID_SOCKET;
        g_clients[slot].authed = 0;
        g_clients[slot].addr_str[0] = '\0';
    }
}

static void clients_cleanup(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++)
        client_drop(i);
}

/*
 * Process one action from a multiplexed client.
 * If not yet authenticated, handles the AUTH frame.
 * If authenticated, handles one command frame.
 * Returns 0 to keep the client, -1 to drop it.
 */
static int client_process(int slot)
{
    client_slot_t *cl = &g_clients[slot];
    char *buf;
    DWORD len;

    if (!cl->authed) {
        /* Auth frame expected */
        if (auth_verify(cl->sock, g_secret) != 0) {
            log_msg(LOG_MAIN, "Auth failed for slot %d (%s)",
                    slot, cl->addr_str);
            return -1;
        }
        cl->authed = 1;
        log_msg(LOG_MAIN, "Client %d (%s) authenticated", slot, cl->addr_str);
        return 0;
    }

    /* Command frame */
    if (frame_recv(cl->sock, &buf, &len) != 0)
        return -1;  /* connection lost */

    if (len == 0) {
        HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }

    {
        char preview[81];
        DWORD plen = len < 80 ? len : 80;
        memcpy(preview, buf, plen);
        preview[plen] = '\0';
        log_msg(LOG_MAIN, "[%d] CMD: \"%s\"%s (%lu bytes)",
                slot, preview, len > 80 ? "..." : "",
                (unsigned long)len);
    }

    {
        handler_state_t *hs = handler_state();
        if (!hs) {
            /* TLS unavailable (extremely unlikely) — run without SEH recovery */
            handle_command(cl->sock, buf, len);
        } else if (setjmp(hs->handler_jmp) == 0) {
            hs->in_handler = 1;
            handle_command(cl->sock, buf, len);
            hs->in_handler = 0;
        } else {
            hs->in_handler = 0;
            log_msg(LOG_MAIN, "[%d] Exception 0x%08lX processing command",
                    slot, (unsigned long)hs->exception_code);
            send_error_response(cl->sock, "Internal error: exception in handler");
        }
    }

    HeapFree(GetProcessHeap(), 0, buf);
    return 0;
}

/*
 * Handle a single client session (auth + command loop).
 * Used by single-client (-1) and threaded (-t) modes.
 */
static void handle_client(SOCKET client)
{
    char *cmd_buf;
    DWORD cmd_len;

    /* SO_RCVTIMEO crashes Win98 Winsock — skip on Win9x.
     * On NT (XP+), set receive timeout to detect dead clients. */
    {
        OSVERSIONINFOA osvi;
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        GetVersionExA(&osvi);
        if (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT) {
            DWORD recv_timeout = 120000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                       (const char *)&recv_timeout, sizeof(recv_timeout));
        }
    }

    log_msg(LOG_MAIN, "handle_client: starting auth on socket %u",
            (unsigned)client);

    /* First frame must be AUTH */
    if (auth_verify(client, g_secret) != 0) {
        log_msg(LOG_MAIN, "Auth failed, closing connection");
        closesocket(client);
        return;
    }
    log_msg(LOG_MAIN, "Client authenticated");

    /* Command loop */
    while (g_running) {
        if (frame_recv(client, &cmd_buf, &cmd_len) != 0)
            break;  /* connection lost */

        if (cmd_len == 0) {
            HeapFree(GetProcessHeap(), 0, cmd_buf);
            continue;
        }

        {
            /* Log first 80 chars of command */
            char preview[81];
            DWORD plen = cmd_len < 80 ? cmd_len : 80;
            memcpy(preview, cmd_buf, plen);
            preview[plen] = '\0';
            log_msg(LOG_MAIN, "CMD: \"%s\"%s (%lu bytes)",
                    preview, cmd_len > 80 ? "..." : "",
                    (unsigned long)cmd_len);
        }

        /* Watchdog instrumentation: mark a command in-flight so watchdog_thread
         * can recover the agent if a handler wedges behind a hung fullscreen
         * game (Glide display lock). Incremented before setjmp so a handler that
         * longjmps out still reaches the matching decrement below. */
        g_cmd_start = GetTickCount();
        InterlockedIncrement(&g_cmd_inflight);
        {
            handler_state_t *hs = handler_state();
            if (!hs) {
                handle_command(client, cmd_buf, cmd_len);
            } else if (setjmp(hs->handler_jmp) == 0) {
                hs->in_handler = 1;
                handle_command(client, cmd_buf, cmd_len);
                hs->in_handler = 0;
            } else {
                hs->in_handler = 0;
                log_msg(LOG_MAIN, "Exception 0x%08lX processing command, "
                        "continuing", (unsigned long)hs->exception_code);
                send_error_response(client, "Internal error: exception in handler");
            }
        }
        InterlockedDecrement(&g_cmd_inflight);
        HeapFree(GetProcessHeap(), 0, cmd_buf);

        if (!g_running) break;
    }

    log_msg(LOG_MAIN, "Client disconnected");
    closesocket(client);
}

/*
 * Client handler thread - one per connected controller (threaded mode).
 */
static DWORD WINAPI client_thread(LPVOID param)
{
    SOCKET client = (SOCKET)(UINT_PTR)param;
    handle_client(client);
    /* Free this thread's lazily-allocated handler state (native TLS isn't
     * auto-reclaimed on thread exit the way __thread was). */
    if (g_hs_tls != TLS_OUT_OF_INDEXES) {
        void *hs = TlsGetValue(g_hs_tls);
        if (hs) {
            HeapFree(GetProcessHeap(), 0, hs);
            TlsSetValue(g_hs_tls, NULL);
        }
    }
    return 0;
}

/*
 * Turn on batched logging once the startup danger has passed.
 *
 * Everything the agent does that has historically killed it happens in the
 * first couple of minutes - the dosstage payload copy at ~45s being the worst
 * - and those lines have to be on disk per-line to be any use afterwards.
 * After that the agent is idle chatter, which is what batching is for.
 */
#define LOG_BUFFER_AFTER_MS  120000

static DWORD WINAPI delayed_buffering_thread(LPVOID unused)
{
    int slept = 0;
    (void)unused;
    while (slept < LOG_BUFFER_AFTER_MS && g_running) {
        Sleep(1000);
        slept += 1000;
    }
    if (!g_running) return 0;
    log_set_buffered(1);
    log_msg(LOG_MAIN, "log: batched writes on (flush when full or every 15s) "
            "- startup window is past");
    return 0;
}

static BOOL WINAPI console_handler(DWORD ctrl_type)
{
    /* On Win9x this agent is a console window and closing that window IS how
     * it usually stops - so this is the last code that runs. Get the batched
     * log out before the process goes. Also covers logoff/shutdown, which is
     * where a reboot's final lines would otherwise be lost. */
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT ||
        ctrl_type == CTRL_LOGOFF_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        log_msg(LOG_MAIN, "console control event %lu (power_pending=%d)",
                (unsigned long)ctrl_type, g_power_pending);
        log_flush();

        /*
         * Getting the log out is always right. STOPPING is not.
         *
         * LOGOFF and SHUTDOWN are exactly the events Win9x raises while it is
         * tearing the session down - including the teardown WE asked for via
         * REBOOT. Exiting here would kill the agent mid-negotiation and
         * cancel that shutdown, which is precisely the bug do_system_power()
         * was just fixed to avoid; honouring these events would quietly
         * re-arm it from a second direction. During a power operation we stay
         * alive and let the OS terminate us.
         */
        if (!g_power_pending)
            g_running = 0;
        return TRUE;
    }
    return FALSE;
}

/* Dump the full hardware/OS metadata to the log — everything needed to
 * analyze an issue off the box (this is what the share-copied log carries).
 * Uses only direct APIs (no dependency on cache_system_info), so it can run
 * very early. Note the CPU family line: `family=5` is a plain Pentium (no
 * CMOV) — exactly the fact behind the Deskpro 2000 illegal-instruction saga. */
static void log_system_metadata(void)
{
    SYSTEM_INFO si;
    OSVERSIONINFOA osvi;
    MEMORYSTATUS ms;
    DISPLAY_DEVICEA dd;
    char host[128];
    DWORD hlen = sizeof(host);

    GetSystemInfo(&si);
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    if (!GetComputerNameA(host, &hlen))
        safe_strncpy(host, "?", sizeof(host));

    log_msg(LOG_MAIN, "META agent=%s host=%s", AGENT_VERSION, host);
    log_msg(LOG_MAIN, "META os=%lu.%lu.%lu platformId=%lu (%s) csd=\"%s\"",
            (unsigned long)osvi.dwMajorVersion,
            (unsigned long)osvi.dwMinorVersion,
            (unsigned long)osvi.dwBuildNumber,
            (unsigned long)osvi.dwPlatformId,
            osvi.dwPlatformId == VER_PLATFORM_WIN32_NT ? "NT" : "9x",
            osvi.szCSDVersion);
    log_msg(LOG_MAIN, "META cpu arch=%u family=%u model=%u stepping=%u "
            "ncpu=%lu type=%lu%s",
            si.wProcessorArchitecture, si.wProcessorLevel,
            (unsigned)((si.wProcessorRevision >> 8) & 0xff),
            (unsigned)(si.wProcessorRevision & 0xff),
            (unsigned long)si.dwNumberOfProcessors,
            (unsigned long)si.dwProcessorType,
            si.wProcessorLevel < 6 ? " (pre-i686: NO CMOV)" : "");
    log_msg(LOG_MAIN, "META ram_total=%luMB ram_avail=%luMB",
            (unsigned long)(ms.dwTotalPhys / (1024 * 1024)),
            (unsigned long)(ms.dwAvailPhys / (1024 * 1024)));
    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesA(NULL, 0, &dd, 0))
        log_msg(LOG_MAIN, "META gpu=\"%s\" id=\"%s\"",
                dd.DeviceString, dd.DeviceID);
}

/* Best-effort mirror of the local agent.log to the file share, so logs from a
 * box that can't be reached interactively (or that crashed) can be pulled from
 * one place. Copies to <share>\agent logs\<host>-agent.log (+ .1 backup) if the
 * share is reachable; silently no-ops when it isn't ("if the network is
 * accessible"). Per-host filename so boxes don't collide. Runs the FIRST copy
 * soon after boot (so the PREVIOUS run's log, incl. any crash that persisted
 * to the local file, gets uploaded), then periodically. */
#define SHARELOG_DIR_DEFAULT \
    "\\\\192.168.1.122\\files\\Utility\\Retro Automation\\agent logs"
#define SHARELOG_FIRST_MS   10000
#define SHARELOG_PERIOD_MS  60000

/* Start a fire-and-forget helper thread WITHOUT leaking its handle.
 * CreateThread's handle is a kernel object in its own right: discarding it
 * (as every call here used to) leaks one per thread started, and on Win9x
 * those are not free. Closing the handle does not stop the thread. */
static int spawn_helper(LPTHREAD_START_ROUTINE fn, const char *what)
{
    DWORD tid;
    /*
     * &tid IS NOT OPTIONAL. On Windows 95/98 CreateThread REQUIRES a non-NULL
     * lpThreadId; only NT allows NULL. Every helper here used to pass NULL,
     * so on the Win98 box every one of them failed with ERROR_INVALID_PARAMETER
     * (87) and simply never ran - automap, autoupdate, retrowall, watchdog,
     * ai_status, sharelog and dosstage alike. Only dosstage checked its return
     * value, so only dosstage ever said so, and its message guessed
     * "(low memory?)" - which sent us looking at RAM on a box that had 87MB
     * free. It is why auto-update did nothing on that machine for four
     * versions, why its log was never mirrored to the share, and why the share
     * had to be mapped by hand.
     *
     * Do not "simplify" this back to NULL.
     */
    HANDLE h = CreateThread(NULL, 0, fn, NULL, 0, &tid);
    if (!h) {
        DWORD err = GetLastError();
        log_msg(LOG_MAIN, "%s thread FAILED to start: %lu%s", what,
                (unsigned long)err,
                err == ERROR_INVALID_PARAMETER
                    ? " (ERROR_INVALID_PARAMETER - on Win9x lpThreadId must "
                      "not be NULL)"
                    : err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_OUTOFMEMORY
                    ? " (out of memory)" : "");
        return 0;
    }
    CloseHandle(h);
    return 1;
}

static DWORD WINAPI sharelog_thread(LPVOID param)
{
    char dir[512], dest[640], srcbak[MAX_PATH + 8], destbak[680];
    char host[128];
    DWORD hlen = sizeof(host);
    HKEY hk;
    int i;
    (void)param;

    if (!GetComputerNameA(host, &hlen))
        safe_strncpy(host, "agent", sizeof(host));

    /* Destination dir: registry override HKLM\Software\RetroAgent\ShareLogDir,
     * else the default UNC. */
    safe_strncpy(dir, SHARELOG_DIR_DEFAULT, sizeof(dir));
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0,
                      KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        DWORD ty = REG_SZ, n = sizeof(dir);
        RegQueryValueExA(hk, "ShareLogDir", NULL, &ty, (BYTE *)dir, &n);
        RegCloseKey(hk);
    }
    _snprintf(dest, sizeof(dest), "%s\\%s-agent.log", dir, host);
    dest[sizeof(dest) - 1] = '\0';
    _snprintf(destbak, sizeof(destbak), "%s\\%s-agent.log.1", dir, host);
    destbak[sizeof(destbak) - 1] = '\0';
    _snprintf(srcbak, sizeof(srcbak), "%s.1", log_path());
    srcbak[sizeof(srcbak) - 1] = '\0';

    Sleep(SHARELOG_FIRST_MS);
    while (g_running) {
        /* Commit the batch first: the whole point of mirroring is that the
         * copy on the share is what gets read when the box is unreachable,
         * and a copy missing the newest lines is worse than useless. */
        log_flush();
        /* CopyFileA fails fast + harmlessly if the share isn't reachable. */
        if (CopyFileA(log_path(), dest, FALSE))
            log_msg(LOG_MAIN, "sharelog: mirrored to %s", dest);
        if (GetFileAttributesA(srcbak) != INVALID_FILE_ATTRIBUTES)
            CopyFileA(srcbak, destbak, FALSE);
        for (i = 0; i < SHARELOG_PERIOD_MS / 1000 && g_running; i++)
            Sleep(1000);
    }
    return 0;
}

/*
 * agent_run - Core agent loop.
 * Called from main() in console mode, or from ServiceMain in service mode.
 * Initializes Winsock, starts discovery, runs TCP accept loop.
 */

/* Thread wrapper for automap_run_all() — matches LPTHREAD_START_ROUTINE */
static DWORD WINAPI automap_thread_proc(LPVOID param)
{
    (void)param;
    automap_run_all();
    return 0;
}

void agent_run(void)
{
    WSADATA wsa;
    SOCKET listen_sock, listen_sock_alt;
    struct sockaddr_in server_addr, server_addr_alt, client_addr;
    int client_len;
    HANDLE disc_thread;

    log_msg(LOG_MAIN, "Retro Remote Agent Version %s starting (mode=%s)%s%s",
            AGENT_VERSION,
            g_service_mode ? "service" : "console",
            g_logfile[0] ? ", logfile=" : "", g_logfile);

    /* Real-time responsiveness: a fullscreen D3D/Glide benchmark at normal
     * priority starves the agent for 30-60s stretches, making the box look
     * frozen to the controller exactly when diagnostics matter most.  HIGH
     * class keeps command handling live during tests; the agent is idle
     * (select() with 1s timeout) whenever nothing is asked of it, so this
     * costs the foreground app nothing measurable. */
    if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        log_msg(LOG_MAIN, "Process priority raised to HIGH");
    else
        log_msg(LOG_MAIN, "SetPriorityClass(HIGH) failed: %lu",
                (unsigned long)GetLastError());
    if (!g_service_mode) {
        char title[128];
        _snprintf(title, sizeof(title),
                  "Retro Remote Agent Version %s", AGENT_VERSION);
        SetConsoleTitleA(title);
        printf("%s\n", title);
    }

    /* Init Winsock 2 */
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_msg(LOG_MAIN, "WSAStartup failed: %d", WSAGetLastError());
        if (!g_service_mode)
            printf("WSAStartup failed: %d\n", WSAGetLastError());
        return;
    }

    if (!g_service_mode)
        SetConsoleCtrlHandler(console_handler, TRUE);

    log_msg(LOG_MAIN, "startup: cache_system_info()");
    cache_system_info();
    log_msg(LOG_MAIN, "startup: ensure_firewall_exception()");
    ensure_firewall_exception();

    log_msg(LOG_MAIN, "Hostname=%s IP=%s OS=%s RAM=%luMB",
            g_hostname, g_local_ip, g_os_str, (unsigned long)g_ram_mb);
    if (!g_service_mode)
        printf("Hostname: %s  IP: %s  OS: %s  RAM: %luMB\n",
               g_hostname, g_local_ip, g_os_str, (unsigned long)g_ram_mb);

    /* Start discovery broadcaster */
    {
        /* &disc_tid, not NULL: Win9x rejects a NULL lpThreadId, which is why
         * UDP discovery never came up on the Win98 box - :9899 refused
         * connections on every probe because this thread never started. */
        DWORD disc_tid;
        disc_thread = CreateThread(NULL, 0, discovery_thread, NULL, 0, &disc_tid);
        if (!disc_thread)
            log_msg(LOG_MAIN, "discovery thread FAILED to start: %lu",
                    (unsigned long)GetLastError());
    }

    /* Create TCP listening socket */
    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        log_msg(LOG_MAIN, "socket() failed: %d", WSAGetLastError());
        if (!g_service_mode)
            printf("socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return;
    }

    {
        BOOL reuse = TRUE;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&reuse, sizeof(reuse));
        /* keep the listener out of child processes: an orphaned restart/
         * update batch inheriting this handle blocks the next agent's bind
         * (seen on .143 during the v1.9.0 rollout). Resolved dynamically —
         * SetHandleInformation is Win2000+ only, see util.c. */
        set_handle_noinherit((HANDLE)listen_sock);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(AGENT_TCP_PORT);

    if (bind(listen_sock, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == SOCKET_ERROR) {
        log_msg(LOG_MAIN, "bind() failed: %d", WSAGetLastError());
        if (!g_service_mode)
            printf("bind() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return;
    }

    if (listen(listen_sock, 4) == SOCKET_ERROR) {
        log_msg(LOG_MAIN, "listen() failed: %d", WSAGetLastError());
        if (!g_service_mode)
            printf("listen() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return;
    }

    /* Create secondary TCP listening socket (for direct script access) */
    listen_sock_alt = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_alt != INVALID_SOCKET) {
        BOOL reuse = TRUE;
        setsockopt(listen_sock_alt, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&reuse, sizeof(reuse));
        set_handle_noinherit((HANDLE)listen_sock_alt);
        memset(&server_addr_alt, 0, sizeof(server_addr_alt));
        server_addr_alt.sin_family = AF_INET;
        server_addr_alt.sin_addr.s_addr = INADDR_ANY;
        server_addr_alt.sin_port = htons(AGENT_TCP_PORT_ALT);
        if (bind(listen_sock_alt, (struct sockaddr *)&server_addr_alt,
                 sizeof(server_addr_alt)) == SOCKET_ERROR ||
            listen(listen_sock_alt, 4) == SOCKET_ERROR) {
            log_msg(LOG_MAIN, "Alt port %d bind/listen failed: %d",
                    AGENT_TCP_PORT_ALT, WSAGetLastError());
            closesocket(listen_sock_alt);
            listen_sock_alt = INVALID_SOCKET;
        }
    }

    {
        const char *mode_str = "multiplex";
        if (g_client_mode == MODE_SINGLE) mode_str = "single";
        else if (g_client_mode == MODE_THREADED) mode_str = "threaded";
        log_msg(LOG_MAIN, "Listening on TCP :%d%s, discovery on UDP :%d, client_mode=%s",
                AGENT_TCP_PORT,
                listen_sock_alt != INVALID_SOCKET ? "+:9897" : "",
                AGENT_UDP_PORT, mode_str);
        if (!g_service_mode)
            printf("Listening on TCP :%d%s, discovery on UDP :%d (%s)\n",
                   AGENT_TCP_PORT,
                   listen_sock_alt != INVALID_SOCKET ? "+:9897" : "",
                   AGENT_UDP_PORT, mode_str);
    }

    /* Signal service manager that we're fully initialized */
    service_report_running();

    /* Apply system fixes (vcache, autologon, DMA, etc.) */
    /* Multiplex (Win9x): one thread serves all clients, so a long-poll must
     * never hold it for its full timeout. */
    if (g_client_mode == MODE_MULTIPLEX) {
        g_longpoll_max_ms = 1000;
        log_msg(LOG_MAIN, "multiplex mode: long-polls clamped to %dms "
                          "so one client cannot stall the others",
                g_longpoll_max_ms);
    }

    log_msg(LOG_MAIN, "startup: sysfix_apply_startup()");
    sysfix_apply_startup();

    /* Background helper threads. Each is logged as it's spawned so that if the
     * crash is in one of them (or in spawning it), the last breadcrumb names
     * it — important on Win9x where the unhandled-exception filter is not
     * reliably called. */
    log_msg(LOG_MAIN, "startup: spawning automap thread");
    spawn_helper(automap_thread_proc, "automap");

    log_msg(LOG_MAIN, "startup: spawning autoupdate thread");
    spawn_helper(autoupdate_thread, "autoupdate");

    log_msg(LOG_MAIN, "startup: spawning retrowall thread");
    spawn_helper(retrowall_thread, "retrowall");

    /* DOS-capable boxes (Win9x/ME) get the DOS programs staged to C:\ so
     * they're already there when the user boots to DOS. Exits immediately
     * on the NT family. */
    log_msg(LOG_MAIN, "startup: spawning dosstage thread");
    /* CreateThread failures were silent. On a 31MB Win98 box reporting 0MB
     * available, a helper thread can simply fail to start — and then the
     * feature "does nothing" with no trace at all, which is what sent us
     * hunting on the Deskpro. Say so. */
    /* spawn_helper() already logs the failure with the error code. */
    spawn_helper(dosstage_thread, "dosstage");

    /* (Onboarding used to be spawned around here, then was moved off the boot
     * path because on a Pentium 1 its first-boot SMB copy saturated the box
     * for minutes and made the agent look hung. It is gone entirely as of
     * v1.71.0: GAMESYNC below does the same work from the staged library and
     * gates each title on what this machine can actually run.) */

    /* Game index: the host's server-favorites pipeline polls this every few
     * minutes, so the scan has to have already happened by the time it asks.
     * gameindex_init() must run on this thread — the handler and the scanner
     * share a critical section and both may touch it first. */
    log_msg(LOG_MAIN, "startup: spawning gameindex thread");
    gameindex_init();
    spawn_helper(gameindex_thread, "gameindex");

    /* gamesync provisions the game library onto a freshly installed box.
     * Its thread exits immediately when the marker file says this machine
     * has already been done, so it costs nothing on an established box. */
    log_msg(LOG_MAIN, "startup: spawning gamesync thread");
    gamesync_init();
    spawn_helper(gamesync_thread, "gamesync");

    log_msg(LOG_MAIN, "startup: spawning watchdog thread");
    spawn_helper(watchdog_thread, "watchdog");

    log_msg(LOG_MAIN, "startup: spawning ai_status thread");
    spawn_helper(ai_status_thread, "ai_status");

    log_msg(LOG_MAIN, "startup: spawning sharelog thread");
    spawn_helper(sharelog_thread, "sharelog");

    log_msg(LOG_MAIN, "startup: helper threads spawned; entering accept loop");
    /* Batching starts LATER, not here. "Helper threads spawned" is not the
     * end of the risky window: the threads just started are the ones that
     * have actually killed this agent - dosstage copying an 11MB payload
     * ~45s in took the Deskpro down outright, and that looked for hours like
     * a startup crash. Those minutes must stay on-disk-per-line. A tiny
     * thread flips the switch once they are safely past. */
    {
        DWORD tid;
        HANDLE h = CreateThread(NULL, 0, delayed_buffering_thread, NULL, 0, &tid);
        if (h) CloseHandle(h);
        else {
            log_msg(LOG_MAIN, "log: could not start the buffering timer - "
                    "staying unbuffered");
        }
    }
    clients_init();

    /* Accept loop */
    while (g_running) {
        SOCKET client;
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(listen_sock, &readfds);
        if (listen_sock_alt != INVALID_SOCKET)
            FD_SET(listen_sock_alt, &readfds);

        if (g_client_mode == MODE_MULTIPLEX) {
            /* Also select on all connected client sockets */
            int i;
            for (i = 0; i < MAX_CLIENTS; i++)
                if (g_clients[i].sock != INVALID_SOCKET)
                    FD_SET(g_clients[i].sock, &readfds);
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(0, &readfds, NULL, NULL, &tv) <= 0)
            continue;

        /* ---- Accept new connections (primary or alt port) ---- */
        if (FD_ISSET(listen_sock, &readfds) ||
            (listen_sock_alt != INVALID_SOCKET && FD_ISSET(listen_sock_alt, &readfds))) {
            SOCKET accept_sock = FD_ISSET(listen_sock, &readfds) ? listen_sock : listen_sock_alt;
            client_len = sizeof(client_addr);
            client = accept(accept_sock, (struct sockaddr *)&client_addr,
                            &client_len);
            if (client != INVALID_SOCKET) {
                /* Disable Nagle — critical for low-latency small commands */
                {
                    BOOL nodelay = TRUE;
                    setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                               (const char *)&nodelay, sizeof(nodelay));
                }
                log_msg(LOG_MAIN, "Connection from %s:%d",
                        inet_ntoa(client_addr.sin_addr),
                        ntohs(client_addr.sin_port));
                if (!g_service_mode)
                    printf("Connection from %s:%d\n",
                           inet_ntoa(client_addr.sin_addr),
                           ntohs(client_addr.sin_port));

                if (g_client_mode == MODE_SINGLE) {
                    handle_client(client);
                } else if (g_client_mode == MODE_THREADED) {
                    /* Close the handle immediately - it is the HANDLE that
                     * leaks, not the thread, and this path runs once per
                     * connection for the life of the agent. */
                    DWORD ctid;
                    HANDLE th = CreateThread(NULL, 0, client_thread,
                                             (LPVOID)(UINT_PTR)client, 0, &ctid);
                    if (th) CloseHandle(th);
                    else {
                        log_msg(LOG_MAIN, "could not start a client thread - "
                                "dropping the connection");
                        closesocket(client);
                    }
                } else {
                    /* MODE_MULTIPLEX: add to client array */
                    int slot = clients_find_free();
                    if (slot >= 0) {
                        g_clients[slot].sock = client;
                        g_clients[slot].authed = 0;
                        g_clients[slot].last_active = GetTickCount();
                        _snprintf(g_clients[slot].addr_str,
                                  sizeof(g_clients[slot].addr_str),
                                  "%s:%d",
                                  inet_ntoa(client_addr.sin_addr),
                                  ntohs(client_addr.sin_port));
                        log_msg(LOG_MAIN, "Assigned slot %d (%d/%d active)",
                                slot, clients_count(), MAX_CLIENTS);
                    } else {
                        log_msg(LOG_MAIN, "Max clients (%d) reached, rejecting",
                                MAX_CLIENTS);
                        send_error_response(client, "ERR max connections reached");
                        closesocket(client);
                    }
                }
            }
        }

        /* ---- Process data from connected clients (multiplex mode) ---- */
        if (g_client_mode == MODE_MULTIPLEX) {
            int i;
            for (i = 0; i < MAX_CLIENTS && g_running; i++) {
                if (g_clients[i].sock == INVALID_SOCKET)
                    continue;
                if (!FD_ISSET(g_clients[i].sock, &readfds))
                    continue;

                g_clients[i].last_active = GetTickCount();
                if (client_process(i) != 0)
                    client_drop(i);
            }

            /* Reap the silent ones. A half-open connection never reports
             * itself closed, so without this a slot is held for good. */
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (g_clients[i].sock == INVALID_SOCKET) continue;
                if ((DWORD)(GetTickCount() - g_clients[i].last_active)
                        < (DWORD)CLIENT_IDLE_MS)
                    continue;
                log_msg(LOG_MAIN, "slot %d (%s) idle %d s - dropping it",
                        i, g_clients[i].addr_str, CLIENT_IDLE_MS / 1000);
                client_drop(i);
            }
        }
    }

    clients_cleanup();

    if (!g_service_mode)
        printf("Shutting down...\n");
    log_msg(LOG_MAIN, "Shutting down");
    closesocket(listen_sock);
    /* The alt listener used to be left bound. A QUIT then produced a
     * half-dead agent: the process lingered (a helper thread still running),
     * :9897 kept ACCEPTING connections, and nothing ever serviced them — so
     * the box looked reachable but answered nothing, and there was no way
     * back in remotely. Hardware-confirmed on the Deskpro, 2026-07-29. */
    if (listen_sock_alt != INVALID_SOCKET)
        closesocket(listen_sock_alt);
    WaitForSingleObject(disc_thread, 3000);
    if (disc_thread) CloseHandle(disc_thread);   /* the last handle we hold */
    WSACleanup();

    /* Log the clean-exit marker BEFORE closing: log_shutdown() invalidates
     * the file handle, and anything logged after it reaches the console but
     * never the file - so a clean QUIT would end at "Shutting down" and read
     * exactly like an agent that was killed. */
    log_msg(LOG_MAIN, "shutdown complete; exiting process");
    /* Flush and close: the hard exit below deliberately does not unwind, so
     * nothing else would get the batched lines out. */
    log_shutdown();

    /* Let a replacement start immediately rather than waiting for the OS to
     * notice we are gone. */
    if (g_instance_mutex) {
        ReleaseMutex(g_instance_mutex);
        CloseHandle(g_instance_mutex);
        g_instance_mutex = NULL;
    }

    /*
     * Guarantee the process actually dies. Helper threads (retrowall,
     * dosstage, watchdog) and a wedged handler must never be able to keep a
     * quit agent alive holding its ports — an unreachable-but-listening
     * agent on a Win9x box needs physical access to fix.
     * (The exit marker is logged above, before log_shutdown() closes the
     * file — logging it here would only reach the console.)
     */
    ExitProcess(0);
}

int main(int argc, char *argv[])
{
    int i;

    /* Allocate the per-thread handler-state TLS slot before the exception
     * filter (which reads it) can ever run. */
    g_hs_tls = TlsAlloc();

    /* Install the crash filter + suppress fault dialogs as the FIRST thing,
     * before any other startup work, so a fault anywhere in startup is caught
     * and logged rather than silently killing the process. */
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(command_exception_filter);

    /* Parse command line */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            safe_strncpy(g_secret, argv[++i], sizeof(g_secret));
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            safe_strncpy(g_logfile, argv[++i], sizeof(g_logfile));
        } else if (strcmp(argv[i], "-m") == 0) {
            g_client_mode = MODE_MULTIPLEX;
        } else if (strcmp(argv[i], "-1") == 0) {
            g_client_mode = MODE_SINGLE;
        }
    }

    /* Bring logging up as the very first thing (file logging is on by
     * default now) so even a failure in the earliest startup steps below
     * leaves an on-disk breadcrumb. If the log has NO "main() entered" line
     * after a failed run, the failure was at EXE load (a missing/Win2000+
     * import the loader couldn't resolve) — before any of our code ran. */
    log_init(g_logfile[0] ? g_logfile : NULL);

    /*
     * Refuse to be the second instance.
     *
     * Nothing stopped two agents running at once, and that is what produced
     * the two worst symptoms on the Win98 box. Each start logs
     * "Listening on TCP :9898+:9897", but the second only gets whichever port
     * the first did not take - so the fleet's port answers nothing while an
     * agent is demonstrably running. And killing "the" agent then leaves
     * retro_agent.exe locked by the copy that is still alive, which is
     * exactly what the operator hit trying to replace it.
     *
     * The mutex is released by the OS the moment the holder dies, however it
     * dies, so this cannot lock us out of our own box.
     */
    g_instance_mutex = CreateMutexA(NULL, FALSE, AGENT_INSTANCE_MUTEX);
    if (g_instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        log_msg(LOG_MAIN, "another retro_agent is already running - exiting "
                "rather than fighting it for the ports");
        log_flush();
        if (!g_service_mode)
            printf("A retro_agent is already running on this machine.\n");
        CloseHandle(g_instance_mutex);
        g_instance_mutex = NULL;
        log_shutdown();
        return 1;
    }
    log_msg(LOG_MAIN, "==================================================");
    log_msg(LOG_MAIN, "retro_agent v%s: main() entered", AGENT_VERSION);
    log_msg(LOG_MAIN, "log file: %s (rotating, ~512KB x2)", log_path());
    if (!g_service_mode)
        printf("Logging to %s\n", log_path());

    /* Win9x: GCC __thread TLS may not initialize properly in CreateThread
     * threads, causing handler threads to crash silently.  Fall back to
     * multiplex (single-threaded select loop) on Win9x. */
    {
        OSVERSIONINFOA osvi;
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        GetVersionExA(&osvi);
        log_msg(LOG_MAIN, "platform: %lu.%lu build %lu, platformId=%lu (%s)",
                (unsigned long)osvi.dwMajorVersion,
                (unsigned long)osvi.dwMinorVersion,
                (unsigned long)osvi.dwBuildNumber,
                (unsigned long)osvi.dwPlatformId,
                osvi.dwPlatformId == VER_PLATFORM_WIN32_NT ? "NT" : "9x");
        if (osvi.dwPlatformId != VER_PLATFORM_WIN32_NT
            && g_client_mode == MODE_THREADED) {
            g_client_mode = MODE_MULTIPLEX;
            log_msg(LOG_MAIN, "Win9x: client mode forced to multiplex "
                    "(threaded TLS unsafe on 9x)");
        }
    }

    /* Full hardware/OS metadata up front (also what the share-copied log
     * carries), logged early so it's captured even if startup fails later. */
    log_system_metadata();

    /* (crash filter + error mode were installed at the very top of main) */

    /*
     * Try to run as an NT service. If started by the SCM, this call
     * blocks until the service stops. On Win9x or when started from
     * a console, it returns 0 immediately and we fall through.
     */
    log_msg(LOG_MAIN, "startup: calling try_service_start()");
    if (try_service_start()) {
        log_msg(LOG_MAIN, "ran as NT service; shutting down");
        return 0;
    }
    log_msg(LOG_MAIN, "startup: console mode, entering agent_run()");

    /* Console mode */
    agent_run();
    log_msg(LOG_MAIN, "agent_run() returned; process exiting");
    return 0;
}
