/*
 * package.c - Package management via distro detection.
 * Detects apt/yum/pacman from /etc/os-release, dispatches accordingly.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

typedef enum { PKG_APT, PKG_YUM, PKG_PACMAN, PKG_UNKNOWN } pkg_mgr_t;

static pkg_mgr_t detect_pkg_manager(void)
{
    if (access("/usr/bin/apt-get", X_OK) == 0 || access("/usr/bin/apt", X_OK) == 0)
        return PKG_APT;
    if (access("/usr/bin/yum", X_OK) == 0 || access("/usr/bin/dnf", X_OK) == 0)
        return PKG_YUM;
    if (access("/usr/bin/pacman", X_OK) == 0)
        return PKG_PACMAN;
    return PKG_UNKNOWN;
}

static char *run_command(const char *cmd)
{
    FILE *fp;
    char *output = NULL;
    uint32_t output_len = 0, output_cap = 65536;

    fp = popen(cmd, "r");
    if (!fp) return NULL;

    output = (char *)malloc(output_cap);
    if (!output) { pclose(fp); return NULL; }

    while (1) {
        size_t n;
        if (output_len + 4096 > output_cap) {
            output_cap *= 2;
            output = (char *)realloc(output, output_cap);
            if (!output) { pclose(fp); return NULL; }
        }
        n = fread(output + output_len, 1, 4096, fp);
        if (n == 0) break;
        output_len += (uint32_t)n;
    }
    output[output_len] = '\0';

    pclose(fp);
    return output;
}

void handle_pkginstall(SOCKET sock, const char *args)
{
    pkg_mgr_t mgr;
    char cmd[1024];
    char *output;

    if (!args || !args[0]) {
        send_error_response(sock, "PKGINSTALL requires a package name");
        return;
    }

    mgr = detect_pkg_manager();
    switch (mgr) {
    case PKG_APT:
        snprintf(cmd, sizeof(cmd),
                 "DEBIAN_FRONTEND=noninteractive apt-get install -y %s 2>&1", args);
        break;
    case PKG_YUM:
        snprintf(cmd, sizeof(cmd), "yum install -y %s 2>&1", args);
        break;
    case PKG_PACMAN:
        snprintf(cmd, sizeof(cmd), "pacman -S --noconfirm %s 2>&1", args);
        break;
    default:
        send_error_response(sock, "No supported package manager found");
        return;
    }

    log_msg(LOG_PKG, "PKGINSTALL: %s", cmd);
    output = run_command(cmd);

    if (output) {
        send_text_response(sock, output);
        free(output);
    } else {
        send_error_response(sock, "Package install command failed");
    }
}

void handle_pkglist(SOCKET sock)
{
    pkg_mgr_t mgr;
    const char *cmd;
    char *output;

    mgr = detect_pkg_manager();
    switch (mgr) {
    case PKG_APT:    cmd = "dpkg -l 2>&1"; break;
    case PKG_YUM:    cmd = "rpm -qa 2>&1"; break;
    case PKG_PACMAN: cmd = "pacman -Q 2>&1"; break;
    default:
        send_error_response(sock, "No supported package manager found");
        return;
    }

    output = run_command(cmd);
    if (output) {
        send_text_response(sock, output);
        free(output);
    } else {
        send_error_response(sock, "Package list command failed");
    }
}
