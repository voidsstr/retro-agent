#ifndef HANDLERS_H
#define HANDLERS_H

#include "protocol.h"

/* Command dispatch - routes command string to appropriate handler */
void handle_command(SOCKET sock, const char *cmd, uint32_t cmd_len);

/* Individual command handlers */
void handle_ping(SOCKET sock);
void handle_sysinfo(SOCKET sock);
void handle_screenshot(SOCKET sock, const char *args);
void handle_exec(SOCKET sock, const char *args);
void handle_upload(SOCKET sock, const char *args);
void handle_download(SOCKET sock, const char *args);
void handle_dirlist(SOCKET sock, const char *args);
void handle_mkdir(SOCKET sock, const char *args);
void handle_delete(SOCKET sock, const char *args);
void handle_proclist(SOCKET sock);
void handle_prockill(SOCKET sock, const char *args);
void handle_shutdown(SOCKET sock);
void handle_reboot(SOCKET sock);
void handle_launch(SOCKET sock, const char *args);
void handle_filecopy(SOCKET sock, const char *args);
void handle_pkginstall(SOCKET sock, const char *args);
void handle_pkglist(SOCKET sock);
void handle_svcinstall(SOCKET sock, const char *args);

/* Shared flag for graceful shutdown */
extern volatile int g_running;

#endif /* HANDLERS_H */
