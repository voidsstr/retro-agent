/*
 * handlers.c - Command dispatch and routing for Linux agent.
 * Same dispatch table pattern as Windows agent.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    const char *name;
    int         has_args;
    void       (*handler_no_args)(SOCKET sock);
    void       (*handler_with_args)(SOCKET sock, const char *args);
} cmd_entry_t;

static const cmd_entry_t commands[] = {
    { "PING",       0, handle_ping,       NULL },
    { "SYSINFO",    0, handle_sysinfo,    NULL },
    { "SCREENSHOT", 1, NULL,              handle_screenshot },
    { "EXEC",       1, NULL,              handle_exec },
    { "UPLOAD",     1, NULL,              handle_upload },
    { "DOWNLOAD",   1, NULL,              handle_download },
    { "DIRLIST",    1, NULL,              handle_dirlist },
    { "MKDIR",      1, NULL,              handle_mkdir },
    { "DELETE",     1, NULL,              handle_delete },
    { "PROCLIST",   0, handle_proclist,   NULL },
    { "PROCKILL",   1, NULL,              handle_prockill },
    { "SHUTDOWN",   0, handle_shutdown,   NULL },
    { "REBOOT",     0, handle_reboot,     NULL },
    { "LAUNCH",     1, NULL,              handle_launch },
    { "FILECOPY",   1, NULL,              handle_filecopy },
    { "PKGINSTALL", 1, NULL,              handle_pkginstall },
    { "PKGLIST",    0, handle_pkglist,    NULL },
    { "SVCINSTALL", 1, NULL,              handle_svcinstall },
    { NULL,         0, NULL,              NULL }
};

void handle_command(SOCKET sock, const char *cmd, uint32_t cmd_len)
{
    const cmd_entry_t *entry;
    char cmd_name[32];
    const char *args = NULL;
    int i;

    (void)cmd_len;

    /* Extract command name (first word) */
    for (i = 0; cmd[i] && cmd[i] != ' ' && i < (int)sizeof(cmd_name) - 1; i++)
        cmd_name[i] = cmd[i];
    cmd_name[i] = '\0';

    /* Find args after first space */
    if (cmd[i] == ' ')
        args = str_skip_spaces(cmd + i + 1);

    /* Look up command */
    for (entry = commands; entry->name; entry++) {
        if (strcasecmp(cmd_name, entry->name) == 0) {
            if (entry->has_args) {
                entry->handler_with_args(sock, args ? args : "");
            } else {
                entry->handler_no_args(sock);
            }
            return;
        }
    }

    send_error_response(sock, "Unknown command");
}

/* Simple handlers implemented directly here */

void handle_ping(SOCKET sock)
{
    send_text_response(sock, "PONG");
}

void handle_shutdown(SOCKET sock)
{
    send_text_response(sock, "OK");
    g_running = 0;
}

void handle_reboot(SOCKET sock)
{
    send_text_response(sock, "OK");
    log_msg(LOG_MAIN, "REBOOT: initiating system reboot");
    sync();
    system("reboot");
    g_running = 0;
}
