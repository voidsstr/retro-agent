/*
 * service.c - systemd service management for the Linux agent.
 * SVCINSTALL generates and manages /etc/systemd/system/retro-agent.service
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define SERVICE_NAME    "retro-agent"
#define SERVICE_PATH    "/etc/systemd/system/" SERVICE_NAME ".service"

static const char *service_template =
    "[Unit]\n"
    "Description=NSC Retro Agent (Linux)\n"
    "After=network-online.target\n"
    "Wants=network-online.target\n"
    "\n"
    "[Service]\n"
    "Type=simple\n"
    "ExecStart=%s -s %s -l /var/log/retro-agent.log\n"
    "Restart=always\n"
    "RestartSec=5\n"
    "StandardOutput=journal\n"
    "StandardError=journal\n"
    "\n"
    "[Install]\n"
    "WantedBy=multi-user.target\n";

static char *run_cmd(const char *cmd)
{
    char *out = NULL;
    uint32_t len = 0, cap = 4096;
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    out = (char *)malloc(cap);
    if (!out) { pclose(fp); return NULL; }

    while (1) {
        size_t n;
        if (len + 1024 > cap) {
            cap *= 2;
            out = (char *)realloc(out, cap);
            if (!out) { pclose(fp); return NULL; }
        }
        n = fread(out + len, 1, 1024, fp);
        if (n == 0) break;
        len += (uint32_t)n;
    }
    out[len] = '\0';
    pclose(fp);
    return out;
}

/* SVCINSTALL install|remove|status|enable|disable */
void handle_svcinstall(SOCKET sock, const char *args)
{
    if (!args || !args[0]) {
        send_error_response(sock, "SVCINSTALL requires: install|remove|status|enable|disable");
        return;
    }

    if (str_starts_with(args, "install")) {
        /* Parse: install <binary_path> <secret> */
        char binary_path[512] = "/usr/local/bin/retro_agent_linux";
        char secret[256] = "retro-agent-secret";
        char unit[2048];
        FILE *f;

        sscanf(args + 7, " %511s %255s", binary_path, secret);

        snprintf(unit, sizeof(unit), service_template, binary_path, secret);

        f = fopen(SERVICE_PATH, "w");
        if (!f) {
            char err[256];
            snprintf(err, sizeof(err), "Cannot write %s: %s", SERVICE_PATH, strerror(errno));
            send_error_response(sock, err);
            return;
        }
        fputs(unit, f);
        fclose(f);

        system("systemctl daemon-reload");
        system("systemctl enable " SERVICE_NAME);
        system("systemctl start " SERVICE_NAME);

        log_msg(LOG_SVC, "Service installed and started");
        send_text_response(sock, "OK service installed and started");

    } else if (str_starts_with(args, "remove")) {
        system("systemctl stop " SERVICE_NAME);
        system("systemctl disable " SERVICE_NAME);
        unlink(SERVICE_PATH);
        system("systemctl daemon-reload");

        log_msg(LOG_SVC, "Service removed");
        send_text_response(sock, "OK service removed");

    } else if (str_starts_with(args, "status")) {
        char *output = run_cmd("systemctl status " SERVICE_NAME " 2>&1");
        if (output) {
            send_text_response(sock, output);
            free(output);
        } else {
            send_error_response(sock, "Cannot get service status");
        }

    } else if (str_starts_with(args, "enable")) {
        system("systemctl enable " SERVICE_NAME);
        send_text_response(sock, "OK");

    } else if (str_starts_with(args, "disable")) {
        system("systemctl disable " SERVICE_NAME);
        send_text_response(sock, "OK");

    } else {
        send_error_response(sock, "Unknown SVCINSTALL subcommand (install|remove|status|enable|disable)");
    }
}
