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
void handle_clickshot(SOCKET sock, const char *args);
void handle_exec(SOCKET sock, const char *args);
void handle_execw(SOCKET sock, const char *args);
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
void ui_click_at(int x, int y, int right, int dbl);
void handle_uidrag(SOCKET sock, const char *args);
void handle_uikey(SOCKET sock, const char *args);
void handle_monitor(SOCKET sock, const char *args);
void handle_drvsnapshot(SOCKET sock, const char *args);
void handle_autologin(SOCKET sock, const char *args);
void handle_service(SOCKET sock, const char *args);
void handle_smartinfo(SOCKET sock);
void handle_displaycfg(SOCKET sock, const char *args);
void handle_audioinfo(SOCKET sock);
void handle_sysfix(SOCKET sock, const char *args);
void handle_licstatus(SOCKET sock, const char *args);
void handle_automap(SOCKET sock, const char *args);
void handle_prompt_push(SOCKET sock, const char *args);
void handle_prompt_pop(SOCKET sock);
void handle_prompt_wait(SOCKET sock, const char *args);
void handle_log_append(SOCKET sock, const char *args);
void handle_log_read(SOCKET sock, const char *args);
void handle_log_wait(SOCKET sock, const char *args);
void handle_log_clear(SOCKET sock);
void handle_proxy_get(SOCKET sock);
void handle_proxy_set(SOCKET sock, const char *args);
void handle_status_set(SOCKET sock, const char *args);
void handle_status_get(SOCKET sock);
void handle_status_wait(SOCKET sock, const char *args);

/* Fleet AI transport (ai.c) - proxies to retro-infer --serve on :9896 */
void handle_ai_hello(SOCKET sock);
void handle_ai_restart(SOCKET sock);
void handle_ai_enable(SOCKET sock);
void handle_ai_disable(SOCKET sock);
int  ai_engine_enabled(void);
void handle_model_load(SOCKET sock, const char *args);
void handle_model_unload(SOCKET sock, const char *args);
void handle_model_list(SOCKET sock);
void handle_infer_run(SOCKET sock, const char *args);
void handle_tensor(SOCKET sock, const char *args);
void handle_ai_raw(SOCKET sock, const char *args);
void handle_ai_rawp(SOCKET sock, const char *args);
/* Startup AI readiness status (console + log) */
DWORD WINAPI ai_status_thread(LPVOID param);

/* Auto-map network drives at startup (no socket needed) */
void automap_run_all(void);

/* Silently apply all system fixes at startup */
void sysfix_apply_startup(void);

/* Self-update from network share (runs as background thread) */
DWORD WINAPI autoupdate_thread(LPVOID param);

/* Apply staged retro wallpaper rotation + park desktop icons at startup.
 * retrowall_thread waits briefly for the shell then calls retrowall_apply_startup. */
void retrowall_apply_startup(void);
DWORD WINAPI retrowall_thread(LPVOID param);

/* First-run onboarding: map share, stage core games, apply desktop/theme.
 * No-op once HKLM\Software\RetroAgent\Onboarded is set (or if no payload is
 * staged on the share). Runs as a background thread after the shell settles. */
/* Onboarding is on-demand now (not run at startup). onboard_run(force)
 * performs it; the ONBOARD command triggers it in the background. */
void onboard_run(int force);
void handle_onboard(SOCKET sock, const char *args);

/* Shared flag for graceful shutdown */
extern volatile int g_running;

/* Service mode support (service.c) */
extern int g_service_mode;
int  try_service_start(void);
void service_report_running(void);

/* Core agent loop (main.c) - called from main() or ServiceMain */
void agent_run(void);

/* Watchdog (watchdog.c): recovers the agent when a command wedges behind a
 * hung fullscreen game (Glide lock). main.c sets g_cmd_inflight/g_cmd_start
 * around each handle_command call. */
DWORD WINAPI watchdog_thread(LPVOID param);
extern volatile LONG  g_cmd_inflight;
extern volatile DWORD g_cmd_start;

#endif /* HANDLERS_H */
