#ifndef HANDLERS_H
#define HANDLERS_H

#include <winsock2.h>
#include <windows.h>

/* Command dispatch - routes command string to appropriate handler */
void handle_command(SOCKET sock, const char *cmd, DWORD cmd_len);

/* Individual command handlers */
void handle_ping(SOCKET sock);
void handle_sysinfo(SOCKET sock);
void handle_videodiag(SOCKET sock);
void handle_drivers(SOCKET sock, const char *args);
void handle_screenshot(SOCKET sock, const char *args);
void handle_screendiff(SOCKET sock, const char *args);
void handle_exec(SOCKET sock, const char *args);
void handle_upload(SOCKET sock, const char *args);
void handle_download(SOCKET sock, const char *args);
void handle_dirlist(SOCKET sock, const char *args);
void handle_mkdir(SOCKET sock, const char *args);
void handle_delete(SOCKET sock, const char *args);
void handle_regread(SOCKET sock, const char *args);
void handle_regwrite(SOCKET sock, const char *args);
void handle_regdelete(SOCKET sock, const char *args);
void handle_pciscan(SOCKET sock);
void handle_proclist(SOCKET sock);
void handle_prockill(SOCKET sock, const char *args);
void handle_quit(SOCKET sock);
void handle_shutdown(SOCKET sock);
void handle_reboot(SOCKET sock);
void handle_netmap(SOCKET sock, const char *args);
void handle_netunmap(SOCKET sock, const char *args);
void handle_filecopy(SOCKET sock, const char *args);
void handle_launch(SOCKET sock, const char *args);
void handle_winlist(SOCKET sock);
void handle_uiclick(SOCKET sock, const char *args);
void handle_uidrag(SOCKET sock, const char *args);
void handle_uikey(SOCKET sock, const char *args);
void handle_drvsnapshot(SOCKET sock, const char *args);
void handle_autologin(SOCKET sock, const char *args);
void handle_service(SOCKET sock, const char *args);
void handle_smartinfo(SOCKET sock);
void handle_displaycfg(SOCKET sock, const char *args);
void handle_audioinfo(SOCKET sock);
void handle_sysfix(SOCKET sock, const char *args);
void handle_automap(SOCKET sock, const char *args);

/* Auto-map network drives at startup (no socket needed) */
void automap_run_all(void);

/* Silently apply all system fixes at startup */
void sysfix_apply_startup(void);

/* Self-update from network share (runs as background thread) */
DWORD WINAPI autoupdate_thread(LPVOID param);

/* Shared flag for graceful shutdown */
extern volatile int g_running;

/* Service mode support (service.c) */
extern int g_service_mode;
int  try_service_start(void);
void service_report_running(void);

/* Core agent loop (main.c) - called from main() or ServiceMain */
void agent_run(void);

#endif /* HANDLERS_H */
