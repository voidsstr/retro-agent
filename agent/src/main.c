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
#define MAX_CLIENTS 4

typedef struct {
    SOCKET sock;
    int    authed;
    char   addr_str[24];  /* "x.x.x.x:port" for logging */
} client_slot_t;

static client_slot_t g_clients[MAX_CLIENTS];

/* Cached system info for discovery packets */
static char g_hostname[256]  = "";
static char g_local_ip[64]   = "";
static char g_os_str[64]     = "";
static char g_cpu_str[128]   = "";
static DWORD g_ram_mb        = 0;

volatile int g_running = 1;

/* Exception recovery for command handlers — per-thread state via TLS.
 * __thread (GCC TLS) gives each client thread its own jmp_buf,
 * avoiding corruption when commands run concurrently in threaded mode. */
static __thread jmp_buf g_handler_jmp;
static __thread int g_in_handler = 0;
static __thread DWORD g_exception_code = 0;

static LONG WINAPI command_exception_filter(PEXCEPTION_POINTERS info)
{
    if (g_in_handler) {
        g_exception_code = info->ExceptionRecord->ExceptionCode;
        longjmp(g_handler_jmp, 1);
    }
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

    if (setjmp(g_handler_jmp) == 0) {
        g_in_handler = 1;
        handle_command(cl->sock, buf, len);
        g_in_handler = 0;
    } else {
        g_in_handler = 0;
        log_msg(LOG_MAIN, "[%d] Exception 0x%08lX processing command",
                slot, (unsigned long)g_exception_code);
        send_error_response(cl->sock, "Internal error: exception in handler");
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

        if (setjmp(g_handler_jmp) == 0) {
            g_in_handler = 1;
            handle_command(client, cmd_buf, cmd_len);
            g_in_handler = 0;
        } else {
            g_in_handler = 0;
            log_msg(LOG_MAIN, "Exception 0x%08lX processing command, continuing",
                    (unsigned long)g_exception_code);
            send_error_response(client, "Internal error: exception in handler");
        }
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
    return 0;
}

static BOOL WINAPI console_handler(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        g_running = 0;
        return TRUE;
    }
    return FALSE;
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
    SOCKET listen_sock;
    struct sockaddr_in server_addr, client_addr;
    int client_len;
    HANDLE disc_thread;

    log_msg(LOG_MAIN, "Retro Remote Agent Version %s starting (mode=%s)%s%s",
            AGENT_VERSION,
            g_service_mode ? "service" : "console",
            g_logfile[0] ? ", logfile=" : "", g_logfile);
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

    cache_system_info();
    ensure_firewall_exception();

    log_msg(LOG_MAIN, "Hostname=%s IP=%s OS=%s RAM=%luMB",
            g_hostname, g_local_ip, g_os_str, (unsigned long)g_ram_mb);
    if (!g_service_mode)
        printf("Hostname: %s  IP: %s  OS: %s  RAM: %luMB\n",
               g_hostname, g_local_ip, g_os_str, (unsigned long)g_ram_mb);

    /* Start discovery broadcaster */
    disc_thread = CreateThread(NULL, 0, discovery_thread, NULL, 0, NULL);

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

    {
        const char *mode_str = "multiplex";
        if (g_client_mode == MODE_SINGLE) mode_str = "single";
        else if (g_client_mode == MODE_THREADED) mode_str = "threaded";
        log_msg(LOG_MAIN, "Listening on TCP :%d, discovery on UDP :%d, client_mode=%s",
                AGENT_TCP_PORT, AGENT_UDP_PORT, mode_str);
        if (!g_service_mode)
            printf("Listening on TCP :%d, discovery on UDP :%d (%s)\n",
                   AGENT_TCP_PORT, AGENT_UDP_PORT, mode_str);
    }

    /* Signal service manager that we're fully initialized */
    service_report_running();

    /* Apply system fixes (vcache, autologon, DMA, etc.) */
    sysfix_apply_startup();

    /* Auto-map stored network drives (in background — may block on retry) */
    CreateThread(NULL, 0, automap_thread_proc, NULL, 0, NULL);

    /* Self-update from network share (checks after 15s delay) */
    CreateThread(NULL, 0, autoupdate_thread, NULL, 0, NULL);

    clients_init();

    /* Accept loop */
    while (g_running) {
        SOCKET client;
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(listen_sock, &readfds);

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

        /* ---- Accept new connections ---- */
        if (FD_ISSET(listen_sock, &readfds)) {
            client_len = sizeof(client_addr);
            client = accept(listen_sock, (struct sockaddr *)&client_addr,
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
                    CreateThread(NULL, 0, client_thread,
                                 (LPVOID)(UINT_PTR)client, 0, NULL);
                } else {
                    /* MODE_MULTIPLEX: add to client array */
                    int slot = clients_find_free();
                    if (slot >= 0) {
                        g_clients[slot].sock = client;
                        g_clients[slot].authed = 0;
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

                if (client_process(i) != 0)
                    client_drop(i);
            }
        }
    }

    clients_cleanup();

    if (!g_service_mode)
        printf("Shutting down...\n");
    log_msg(LOG_MAIN, "Shutting down");
    closesocket(listen_sock);
    WaitForSingleObject(disc_thread, 3000);
    WSACleanup();
}

int main(int argc, char *argv[])
{
    int i;

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

    log_init(g_logfile[0] ? g_logfile : NULL);

    /* Win9x: GCC __thread TLS may not initialize properly in CreateThread
     * threads, causing handler threads to crash silently.  Fall back to
     * multiplex (single-threaded select loop) on Win9x. */
    {
        OSVERSIONINFOA osvi;
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        GetVersionExA(&osvi);
        if (osvi.dwPlatformId != VER_PLATFORM_WIN32_NT
            && g_client_mode == MODE_THREADED) {
            g_client_mode = MODE_MULTIPLEX;
        }
    }

    /* Suppress crash dialog popups — log and recover instead */
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(command_exception_filter);

    /*
     * Try to run as an NT service. If started by the SCM, this call
     * blocks until the service stops. On Win9x or when started from
     * a console, it returns 0 immediately and we fall through.
     */
    if (try_service_start())
        return 0;

    /* Console mode */
    agent_run();
    return 0;
}
