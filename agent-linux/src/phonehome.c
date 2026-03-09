/*
 * phonehome.c - WAN phone-home registration via raw POSIX HTTP POST.
 * No libcurl dependency. Plain HTTP only (use a TLS-terminating proxy for HTTPS).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

#include "protocol.h"
#include "phonehome.h"
#include "util.h"
#include "log.h"

/* Globals defined in main.c */
extern char g_dashboard_url[512];
extern int  g_phonehome_interval;
extern char g_secret[256];
extern volatile int g_running;

/* Cached system info from main.c (static in main.c, so we re-read) */
extern char g_hostname_cached[256];
extern char g_os_str_cached[64];
extern char g_cpu_str_cached[128];
extern unsigned long g_ram_mb_cached;

/* Parsed URL components */
typedef struct {
    char host[256];
    int  port;
    char path[512];
} parsed_url_t;

static int parse_url(const char *url, parsed_url_t *out)
{
    const char *p;
    const char *host_start;
    const char *port_start;
    const char *path_start;
    int host_len;

    memset(out, 0, sizeof(*out));
    out->port = 80;

    /* Skip http:// */
    if (str_starts_with(url, "http://"))
        p = url + 7;
    else
        p = url;

    host_start = p;

    /* Find end of host (: or / or NUL) */
    port_start = NULL;
    path_start = NULL;
    while (*p) {
        if (*p == ':' && !port_start) {
            port_start = p + 1;
        } else if (*p == '/') {
            path_start = p;
            break;
        }
        p++;
    }

    /* Extract host */
    if (port_start) {
        host_len = (int)(port_start - 1 - host_start);
    } else if (path_start) {
        host_len = (int)(path_start - host_start);
    } else {
        host_len = (int)strlen(host_start);
    }
    if (host_len <= 0 || host_len >= (int)sizeof(out->host))
        return -1;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    /* Extract port */
    if (port_start) {
        out->port = atoi(port_start);
        if (out->port <= 0 || out->port > 65535)
            out->port = 80;
    }

    /* Extract path */
    if (path_start) {
        safe_strncpy(out->path, path_start, sizeof(out->path));
    } else {
        safe_strncpy(out->path, "/", sizeof(out->path));
    }

    return 0;
}

static char *build_register_json(void)
{
    json_t j;
    json_init(&j);
    json_object_start(&j);
    json_kv_str(&j, "secret", g_secret);
    json_kv_str(&j, "hostname", g_hostname_cached);
    json_kv_str(&j, "ip", "");
    json_kv_int(&j, "port", AGENT_TCP_PORT);
    json_kv_str(&j, "os", g_os_str_cached);
    json_kv_str(&j, "cpu", g_cpu_str_cached);
    json_kv_uint(&j, "ram_mb", g_ram_mb_cached);
    json_kv_str(&j, "os_family", "linux");
    json_object_end(&j);
    return json_finish(&j);
}

/*
 * Perform a raw HTTP/1.1 POST and return the status code (or -1 on error).
 */
static int http_post(const parsed_url_t *url, const char *body, int body_len)
{
    struct addrinfo hints, *res, *rp;
    char port_str[8];
    int sock = -1;
    char header[1024];
    int header_len;
    int sent, n;
    char response[PHONEHOME_MAX_RESPONSE];
    int resp_len = 0;
    int status_code = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%d", url->port);
    if (getaddrinfo(url->host, port_str, &hints, &res) != 0) {
        log_msg(LOG_MAIN, "phonehome: DNS lookup failed for %s", url->host);
        return -1;
    }

    for (rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;

        /* Set a connect timeout via SO_SNDTIMEO */
        {
            struct timeval tv;
            tv.tv_sec = 10;
            tv.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock < 0) {
        log_msg(LOG_MAIN, "phonehome: connect failed to %s:%d",
                url->host, url->port);
        return -1;
    }

    /* Build HTTP request */
    header_len = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        url->path, url->host, url->port, body_len);

    /* Send header */
    sent = 0;
    while (sent < header_len) {
        n = send(sock, header + sent, header_len - sent, 0);
        if (n <= 0) { close(sock); return -1; }
        sent += n;
    }

    /* Send body */
    sent = 0;
    while (sent < body_len) {
        n = send(sock, body + sent, body_len - sent, 0);
        if (n <= 0) { close(sock); return -1; }
        sent += n;
    }

    /* Read response (just need the status line) */
    while (resp_len < (int)sizeof(response) - 1) {
        n = recv(sock, response + resp_len, sizeof(response) - 1 - resp_len, 0);
        if (n <= 0) break;
        resp_len += n;
        /* Once we have the status line, we can stop */
        if (resp_len > 12) break;
    }
    response[resp_len] = '\0';

    close(sock);

    /* Parse "HTTP/1.x NNN" */
    if (resp_len >= 12 && str_starts_with(response, "HTTP/1.")) {
        status_code = atoi(response + 9);
    }

    return status_code;
}

void *phonehome_thread(void *param)
{
    parsed_url_t url;
    char full_url[600];
    int interval;

    (void)param;

    /* Build the full registration URL */
    snprintf(full_url, sizeof(full_url), "%s/api/fleet/register", g_dashboard_url);

    /* Strip trailing slash duplication */
    {
        int len = strlen(g_dashboard_url);
        if (len > 0 && g_dashboard_url[len - 1] == '/') {
            snprintf(full_url, sizeof(full_url), "%sapi/fleet/register",
                     g_dashboard_url);
        }
    }

    if (parse_url(full_url, &url) != 0) {
        log_msg(LOG_MAIN, "phonehome: invalid URL: %s", full_url);
        return NULL;
    }

    interval = g_phonehome_interval;
    if (interval < 10) interval = 10;

    log_msg(LOG_MAIN, "phonehome: starting, URL=%s:%d%s interval=%ds",
            url.host, url.port, url.path, interval);

    while (g_running) {
        char *body = build_register_json();
        if (body) {
            int code = http_post(&url, body, strlen(body));
            if (code == 200) {
                log_msg(LOG_MAIN, "phonehome: registration OK");
            } else {
                log_msg(LOG_MAIN, "phonehome: registration failed (HTTP %d)", code);
            }
            free(body);
        }

        /* Sleep in 1-second increments for clean shutdown */
        {
            int elapsed = 0;
            while (elapsed < interval && g_running) {
                sleep(1);
                elapsed++;
            }
        }
    }

    log_msg(LOG_MAIN, "phonehome: thread exiting");
    return NULL;
}
