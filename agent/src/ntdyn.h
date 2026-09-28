/*
 * ntdyn.h - Win32 entry points that DO NOT EXIST on Windows 9x, resolved at
 *           runtime so the agent EXE still LOADS on a Win98 box.
 *
 * THE BUG THIS EXISTS TO PREVENT (found on .243, a Win98SE Pentium-1, on
 * 2026-08-30): a STATIC import the loader cannot resolve makes the WHOLE
 * PROCESS fail to load - before a single instruction of ours runs. There is no
 * lazy binding to save you and no error anywhere on the box: the agent simply
 * never starts and never writes a log line. That box was stranded on agent
 * 1.30.0 for 48 versions because retrowall.c called the Service Control
 * Manager directly and gamesync.c called CM_Get_DevNode_Status directly, which
 * put seven NT-only names into the import table:
 *
 *     OpenSCManagerA  OpenServiceA  ControlService  QueryServiceStatus
 *     CloseServiceHandle  ChangeServiceConfigA        (advapi32 - 9x has no SCM)
 *     CM_Get_DevNode_Status                (setupapi on NT, cfgmgr32 on 9x)
 *
 * ONE direct call anywhere in the tree is enough to recreate the import, and it
 * looks like perfectly ordinary C - which is why the regression survived so
 * long and why the guard is a PE-import assertion on the BUILT binary
 * (tests/python/test_agent_win9x_imports.py), not a source grep.
 *
 * So: never call any of these directly. Call the ntdyn_* wrapper, and treat
 * "not available" as a normal outcome - on Win9x it means "this Windows has no
 * Service Control Manager", which is a thing to SKIP and log, not to fail on.
 *
 * NOTE service.c keeps its OWN, larger dynamic table (it also needs
 * CreateServiceA / DeleteService / StartServiceCtrlDispatcherA /
 * RegisterServiceCtrlHandlerA / SetServiceStatus / ChangeServiceConfig2A for
 * NT service mode). It has always resolved them dynamically and is not part of
 * this bug; it is left alone deliberately rather than churned.
 */
#ifndef NTDYN_H
#define NTDYN_H

#include <windows.h>
#include <winsvc.h>

/* ---- Service Control Manager (advapi32.dll; absent on Windows 9x) ---- */

/* Non-zero when this Windows has an SCM at all. Check it FIRST: on 9x every
 * wrapper below fails, and the caller should skip its service work and say so
 * in the log rather than reporting a string of individual failures. */
int ntdyn_scm_available(void);

SC_HANDLE ntdyn_OpenSCManagerA(LPCSTR machine, LPCSTR database, DWORD access);
SC_HANDLE ntdyn_OpenServiceA(SC_HANDLE scm, LPCSTR name, DWORD access);
BOOL      ntdyn_CloseServiceHandle(SC_HANDLE h);
BOOL      ntdyn_QueryServiceStatus(SC_HANDLE svc, LPSERVICE_STATUS status);
BOOL      ntdyn_ControlService(SC_HANDLE svc, DWORD control,
                               LPSERVICE_STATUS status);
BOOL      ntdyn_ChangeServiceConfigA(SC_HANDLE svc, DWORD type, DWORD start,
                                     DWORD error_control, LPCSTR path,
                                     LPCSTR load_order_group, LPDWORD tag_id,
                                     LPCSTR dependencies, LPCSTR start_name,
                                     LPCSTR password, LPCSTR display_name);

/* ---- Configuration Manager ---- */

/* CM_Get_DevNode_Status lives in cfgmgr32.dll on Win98SE and in setupapi.dll
 * (and cfgmgr32.dll) on NT, so both are tried. Non-zero when resolved. */
int ntdyn_cm_available(void);

/* Returns CR_SUCCESS (0) on success, CR_FAILURE when the entry point is not
 * available - so an unavailable Config Manager reads exactly like a device
 * whose status could not be read, which every caller already skips. */
DWORD ntdyn_CM_Get_DevNode_Status(PULONG status, PULONG problem,
                                  DWORD devinst, ULONG flags);

/* The devnode tree (1.87.0, the 3dfx rule in agent/shared/drvsafe.h needs a
 * device's parents and a bridge's children). Same homes, same fallback:
 * CR_FAILURE when unavailable, which every caller treats as "no more". */
DWORD ntdyn_CM_Get_Parent(PDWORD parent, DWORD devinst, ULONG flags);
DWORD ntdyn_CM_Get_Child(PDWORD child, DWORD devinst, ULONG flags);
DWORD ntdyn_CM_Get_Sibling(PDWORD sibling, DWORD devinst, ULONG flags);
DWORD ntdyn_CM_Get_Device_IDA(DWORD devinst, char *buf, ULONG len, ULONG flags);

/* ---- CPU accounting (kernel32.dll; Windows XP SP1+ only) ---- */

/* GetSystemTimes: whole-machine idle / kernel (INCLUDING idle) / user time.
 * Returns 1 on success, 0 where this Windows has no such entry point (9x,
 * NT4, 2000, XP RTM) - callers treat 0 as "CPU load unknown", never as idle. */
int ntdyn_GetSystemTimes(FILETIME *idle, FILETIME *kernel, FILETIME *user);

/* ---- Console window (kernel32.dll) ----
 *
 * GetConsoleWindow is Windows 2000+ and GetConsoleProcessList is XP+: Win98's
 * kernel32 exports neither, so a direct call makes the EXE unloadable there
 * (consolewin.c: the agent minimizes its own console at startup). */

/* Non-zero when GetConsoleWindow resolved - i.e. this is NT, not Win9x. */
int  ntdyn_console_window_available(void);

/* The console window, or NULL when unavailable OR when there is no console;
 * ntdyn_console_window_available() tells those two apart. */
HWND ntdyn_GetConsoleWindow(void);

/* How many processes are attached to this console, or 0 when that cannot be
 * asked (9x, Windows 2000). `list` may be NULL only if `count` is 0 - so pass
 * a small buffer; the return value is the total even when it overflows. */
DWORD ntdyn_GetConsoleProcessList(LPDWORD list, DWORD count);

#endif /* NTDYN_H */
