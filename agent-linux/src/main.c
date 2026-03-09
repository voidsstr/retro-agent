/*
 * retro_agent_linux - main.c
 * Entry point for the Linux remote management agent.
 * Runs a TCP server on port 9898 and UDP discovery broadcaster on port 9899.
 * Supports daemon mode via -d flag.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>

#include "protocol.h"
#include "handlers.h"
#include "util.h"
#include "log.h"
#include "phonehome.h"

/* Default shared secret - override via command line */
char g_secret[256] = "retro-agent-secret";
char g_logfile[256] = "";

/* WAN phone-home settings */
char g_dashboard_url[512] = "";
int  g_phonehome_interval = PHONEHOME_INTERVAL;

/* Cached system info for discovery packets */
static char g_hostname[256]  = "";
static char g_local_ip[64]   = "";
static char g_os_str[64]     = "";
static char g_cpu_str[128]   = "";
static unsigned long g_ram_mb = 0;

/* Exported aliases for phonehome.c */
char g_hostname_cached[256]  = "";
char g_os_str_cached[64]     = "";
char g_cpu_str_cached[128]   = "";
unsigned long g_ram_mb_cached = 0;

volatile int g_running = 1;

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

static void cache_system_info(void)
{
    struct utsname uts;
    struct sysinfo si;

    gethostname(g_hostname, sizeof(g_hostname));
    g_hostname[sizeof(g_hostname) - 1] = '\0';
    get_local_ip(g_local_ip, sizeof(g_local_ip));

    uname(&uts);
    snprintf(g_os_str, sizeof(g_os_str), "Linux_%s", uts.release);

    snprintf(g_cpu_str, sizeof(g_cpu_str), "%s_%d_cores",
             uts.machine, (int)sysconf(_SC_NPROCESSORS_ONLN));

    if (sysinfo(&si) == 0) {
        g_ram_mb = (unsigned long)(si.totalram / (1024 * 1024));
    }

    /* Copy to exported aliases for phonehome.c */
    safe_strncpy(g_hostname_cached, g_hostname, sizeof(g_hostname_cached));
    safe_strncpy(g_os_str_cached,   g_os_str,   sizeof(g_os_str_cached));
    safe_strncpy(g_cpu_str_cached,  g_cpu_str,   sizeof(g_cpu_str_cached));
    g_ram_mb_cached = g_ram_mb;
}

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/*
 * UDP Discovery broadcaster thread.
 */
static void *discovery_thread(void *param)
{
    SOCKET udp_sock;
    struct sockaddr_in bcast_addr, bind_addr;
    char packet[512];
    int bcast_enable = 1;

    (void)param;

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock == INVALID_SOCKET) return NULL;

    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST,
               &bcast_enable, sizeof(bcast_enable));
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR,
               &bcast_enable, sizeof(bcast_enable));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(AGENT_UDP_PORT);
    bind(udp_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
    bcast_addr.sin_port = htons(AGENT_UDP_PORT);

    discovery_build_packet(packet, sizeof(packet), g_hostname, g_local_ip,
                           AGENT_TCP_PORT, g_os_str, g_cpu_str, g_ram_mb, "linux");

    while (g_running) {
        /* Send periodic broadcast */
        sendto(udp_sock, packet, strlen(packet), 0,
               (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));

        /* Wait for DISCOVER probes or timeout */
        {
            int elapsed = 0;
            while (elapsed < DISCOVERY_INTERVAL && g_running) {
                fd_set readfds;
                struct timeval tv;

                FD_ZERO(&readfds);
                FD_SET(udp_sock, &readfds);
                tv.tv_sec = 1;
                tv.tv_usec = 0;

                if (select(udp_sock + 1, &readfds, NULL, NULL, &tv) > 0) {
                    char probe[64];
                    struct sockaddr_in from_addr;
                    socklen_t from_len = sizeof(from_addr);
                    int n = recvfrom(udp_sock, probe, sizeof(probe) - 1, 0,
                                     (struct sockaddr *)&from_addr, &from_len);
                    if (n > 0) {
                        probe[n] = '\0';
                        if (strcmp(probe, "DISCOVER") == 0) {
                            sendto(udp_sock, packet, strlen(packet), 0,
                                   (struct sockaddr *)&from_addr, from_len);
                        }
                    }
                }
                elapsed++;
            }
        }
    }

    close(udp_sock);
    return NULL;
}

static void handle_client(SOCKET client)
{
    char *cmd_buf;
    uint32_t cmd_len;

    log_msg(LOG_MAIN, "handle_client: starting auth on socket %d", client);

    if (auth_verify(client, g_secret) != 0) {
        log_msg(LOG_MAIN, "Auth failed, closing connection");
        close(client);
        return;
    }
    log_msg(LOG_MAIN, "Client authenticated");

    while (g_running) {
        if (frame_recv(client, &cmd_buf, &cmd_len) != 0)
            break;

        if (cmd_len == 0) {
            free(cmd_buf);
            continue;
        }

        {
            char preview[81];
            uint32_t plen = cmd_len < 80 ? cmd_len : 80;
            memcpy(preview, cmd_buf, plen);
            preview[plen] = '\0';
            log_msg(LOG_MAIN, "CMD: \"%s\"%s (%u bytes)",
                    preview, cmd_len > 80 ? "..." : "", cmd_len);
        }

        handle_command(client, cmd_buf, cmd_len);
        free(cmd_buf);

        if (!g_running) break;
    }

    log_msg(LOG_MAIN, "Client disconnected");
    close(client);
}

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);  /* Parent exits */

    setsid();

    /* Fork again to prevent terminal reacquisition */
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);

    /* Write PID file */
    {
        FILE *f = fopen("/var/run/retro-agent.pid", "w");
        if (f) {
            fprintf(f, "%d\n", getpid());
            fclose(f);
        }
    }

    /* Redirect stdio */
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
}

int main(int argc, char *argv[])
{
    int i;
    int daemon_mode = 0;
    SOCKET listen_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    pthread_t disc_tid;
    pthread_t ph_tid;
    int phonehome_started = 0;

    /* Parse command line */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            safe_strncpy(g_secret, argv[++i], sizeof(g_secret));
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            safe_strncpy(g_logfile, argv[++i], sizeof(g_logfile));
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            safe_strncpy(g_dashboard_url, argv[++i], sizeof(g_dashboard_url));
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            g_phonehome_interval = atoi(argv[++i]);
            if (g_phonehome_interval < 10) g_phonehome_interval = 10;
        } else if (strcmp(argv[i], "-d") == 0) {
            daemon_mode = 1;
        }
    }

    if (daemon_mode)
        daemonize();

    log_init(g_logfile[0] ? g_logfile : NULL);

    /* Signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    cache_system_info();

    log_msg(LOG_MAIN, "retro_agent_linux starting");
    log_msg(LOG_MAIN, "Hostname=%s IP=%s OS=%s RAM=%luMB",
            g_hostname, g_local_ip, g_os_str, g_ram_mb);

    if (!daemon_mode) {
        printf("retro_agent_linux starting...\n");
        printf("Hostname: %s  IP: %s  OS: %s  RAM: %luMB\n",
               g_hostname, g_local_ip, g_os_str, g_ram_mb);
    }

    /* Start discovery broadcaster */
    pthread_create(&disc_tid, NULL, discovery_thread, NULL);

    /* Start WAN phone-home thread if dashboard URL provided */
    if (g_dashboard_url[0]) {
        log_msg(LOG_MAIN, "Phone-home enabled: %s (interval %ds)",
                g_dashboard_url, g_phonehome_interval);
        if (!daemon_mode)
            printf("Phone-home: %s (every %ds)\n",
                   g_dashboard_url, g_phonehome_interval);
        pthread_create(&ph_tid, NULL, phonehome_thread, NULL);
        phonehome_started = 1;
    }

    /* Create TCP listening socket */
    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        log_msg(LOG_MAIN, "socket() failed: %s", strerror(errno));
        return 1;
    }

    {
        int reuse = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(AGENT_TCP_PORT);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log_msg(LOG_MAIN, "bind() failed: %s", strerror(errno));
        close(listen_sock);
        return 1;
    }

    if (listen(listen_sock, 4) < 0) {
        log_msg(LOG_MAIN, "listen() failed: %s", strerror(errno));
        close(listen_sock);
        return 1;
    }

    log_msg(LOG_MAIN, "Listening on TCP :%d, discovery on UDP :%d",
            AGENT_TCP_PORT, AGENT_UDP_PORT);

    if (!daemon_mode)
        printf("Listening on TCP :%d, discovery on UDP :%d\n",
               AGENT_TCP_PORT, AGENT_UDP_PORT);

    /* Accept loop */
    while (g_running) {
        SOCKET client;
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(listen_sock, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(listen_sock + 1, &readfds, NULL, NULL, &tv) <= 0)
            continue;

        client_len = sizeof(client_addr);
        client = accept(listen_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client == INVALID_SOCKET) continue;

        log_msg(LOG_MAIN, "Connection from %s:%d",
                inet_ntoa(client_addr.sin_addr),
                ntohs(client_addr.sin_port));

        if (!daemon_mode)
            printf("Connection from %s:%d\n",
                   inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port));

        handle_client(client);
    }

    if (!daemon_mode)
        printf("Shutting down...\n");
    log_msg(LOG_MAIN, "Shutting down");
    close(listen_sock);
    pthread_join(disc_tid, NULL);
    if (phonehome_started)
        pthread_join(ph_tid, NULL);

    return 0;
}
