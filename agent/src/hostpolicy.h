/*
 * hostpolicy.h - Is THIS machine one the fleet may reconfigure?
 *
 * The agent exists to turn a machine into a retro fleet box: it applies a
 * theme, a wallpaper, a screensaver and an icon layout, sets autologon, stages
 * games and pins their resolutions. All of that is correct on a Win9x/XP/7
 * fleet box and WRONG on somebody's modern daily-driver PC, which may run the
 * agent only so the fleet can reach it (the copier host .139 is exactly this).
 *
 * So: on Windows 10/11 the agent runs, answers questions and stays reachable,
 * but applies NOTHING to the host. See host_manages_this_box().
 *
 *
 * WHY NOT GetVersionEx() - READ THIS BEFORE CHANGING THE DETECTION
 * ---------------------------------------------------------------
 * `GetVersionEx` reports **6.2 (Windows 8)** on every Windows 8.1, 10 and 11
 * machine unless the calling EXE carries an application manifest listing the
 * newer <supportedOS> GUIDs. retro_agent.exe has no manifest at all - its only
 * resource is the icon (agent/res/retro_agent.rc) - so a `dwMajorVersion >= 10`
 * test would be FALSE on precisely the machines this module exists to protect,
 * and the agent would skin them anyway while the code read as if it could not.
 *
 * Adding a manifest is not the fix either: it changes how every other Windows
 * compatibility shim treats the process, on boxes as old as Win98.
 *
 * `RtlGetVersion` (ntdll) is not subject to that shim and returns the true
 * version. It is the documented way to do this.
 *
 *
 * WHY ntdll IS LoadLibrary'd AND NOT CALLED DIRECTLY
 * --------------------------------------------------
 * A static import the Win98 loader cannot resolve kills the whole process at
 * EXE load, before main(), with nothing written anywhere - the bug that
 * stranded .243 for 48 versions (see ntdyn.h). ntdll.dll is NOT one of the
 * eight DLLs proven to load on Win98SE, and tests/python/test_agent_win9x_imports.py
 * asserts the import table against exactly that set. So RtlGetVersion is
 * resolved with LoadLibrary + GetProcAddress, which adds no import-table entry.
 * On Win9x the load simply fails, we fall back to GetVersionEx, and 9x is
 * never "modern" under any reading. Do not turn this into a direct call.
 */
#ifndef HOSTPOLICY_H
#define HOSTPOLICY_H

#include <windows.h>

/* HKLM\Software\RetroAgent\ManageModernWindows (DWORD).
 *
 * 1 = manage this box anyway, exactly as if it were a retro fleet box. The
 * escape hatch for a modern machine somebody deliberately wants skinned.
 * Absent or 0 (the default) = hands off. */
#define HOSTPOLICY_OVERRIDE_KEY   "Software\\RetroAgent"
#define HOSTPOLICY_OVERRIDE_VALUE "ManageModernWindows"

/* The first Windows major version treated as "modern". 10 covers Windows 10
 * and 11 (Windows 11 still reports major 10; it is identified by build >=
 * 22000, which does not matter here - both are modern). Windows 7 is 6.1 and
 * Windows 8/8.1 are 6.2/6.3, so they stay MANAGED: the fleet has a Win7 box
 * and it is a retro box like any other. */
#define HOSTPOLICY_MODERN_MAJOR   10

/* True version of the running Windows, via RtlGetVersion where available.
 * Any of the out params may be NULL. Returns 1 if a version was determined. */
int host_os_version(DWORD *major, DWORD *minor, DWORD *build);

/* 1 if this is Windows 10/11 (or newer). */
int host_is_modern_windows(void);

/*
 * THE ONE QUESTION EVERY CALLER SHOULD ASK.
 *
 * 1 = this box is a fleet box; apply whatever you were going to apply.
 * 0 = modern Windows without the override; change NOTHING about the host.
 *
 * Result is computed once and cached, so callers in hot paths are free.
 */
int host_manages_this_box(void);

/*
 * Guard for a named piece of host-changing work.
 *
 * Returns 1 if the work must be SKIPPED, having logged why (once per distinct
 * `what`, so a 5-minute keep-loop cannot fill the log). Returns 0 to proceed.
 *
 *     if (host_policy_skip("retrowall")) return;
 */
int host_policy_skip(const char *what);

/* One line for the startup banner / SYSINFO, e.g.
 * "Windows 10/11 (10.0.26100) - host management DISABLED". */
void host_policy_describe(char *buf, int bufsize);

#endif /* HOSTPOLICY_H */
