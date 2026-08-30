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

#endif /* NTDYN_H */
