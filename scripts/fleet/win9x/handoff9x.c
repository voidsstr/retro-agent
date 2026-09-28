/* handoff9x - get a Win9x box onto a given agent build across a RESTART, or
 * guard a RESTART, with no QUIT and no reboot. LAUNCH it, then send RESTART.
 *
 *   handoff9x take  <exe>   make <exe> the NEXT agent to run (e.g. a new build
 *                           under another name: C:\RETRO_AGENT\RA190.EXE)
 *   handoff9x watch <exe>   change nothing unless RESTART fails to bring an
 *                           agent back; then start <exe>
 *
 * Why agentswap9x + RESTART cannot swap (measured on .243, agent 1.86.1,
 * 2026-09-28): after RESTART the old agent RELEASES its single-instance mutex
 * at once but LINGERS in the process list (3 threads) - its console DOS VM
 * still hosts retro_chat.exe - and keeps retro_agent.exe mapped. restart.bat
 * relaunches that same exe ~8 s later, the relaunch takes the free mutex, and
 * the file can never be renamed: agentswap9x, which waits for the PROCESS to
 * go, gave up after 120 s. So this tool watches the MUTEX.
 *
 * take:  1. poll (max 300 s) until the agent's mutex, first seen HELD, is free;
 *           then TAKE it - RESTART's own relaunch finds it held and exits
 *           ("another retro_agent is already running").
 *        2. hold it 20 s so that relaunch has come and gone, release it, start
 *           <exe> minimized in its own console.
 *        3. if <exe> is not still running 25 s later, start
 *           C:\RETRO_AGENT\retro_agent.exe - never leave the box without an agent.
 * watch: 1. as above, wait for the mutex to go from held to free.
 *        2. if an agent holds it again within 90 s, done - nothing started.
 *        3. otherwise start <exe> (plain window) and report whether it runs.
 * Probing uses OpenMutexA, which never creates the object, so it cannot make a
 * starting agent see ERROR_ALREADY_EXISTS; only `take` step 1 creates it.
 * No C runtime (see README: an msvcrt build once left an orphaned dialog on
 * .243). -march=i586. Log: C:\RETRO_AGENT\HANDOFF.TXT */
#include <windows.h>
#define DIR "C:\\RETRO_AGENT\\"
#define AGENT_MUTEX "RetroAgentSingleInstance"   /* agent/src/main.c AGENT_INSTANCE_MUTEX */
static HANDLE out;
static char line[600];
static void w(const char *s)
{
    DWORD n;
    char ts[32];
    wsprintfA(ts, "[%lu ms] ", GetTickCount());
    WriteFile(out, ts, lstrlenA(ts), &n, NULL);
    WriteFile(out, s, lstrlenA(s), &n, NULL);
    WriteFile(out, "\r\n", 2, &n, NULL);
    FlushFileBuffers(out);
}
static void done(int code)
{
    CloseHandle(out);
    ExitProcess(code);
}
/* 1 if some process holds the agent's mutex. Never creates it. */
static int mutex_held(void)
{
    HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, AGENT_MUTEX);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}
static HANDLE start_exe(const char *exe, int minimized)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (minimized) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMINNOACTIVE;
    }
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, DIR, &si, &pi)) {
        wsprintfA(line, "CreateProcess(%s) failed: %lu", exe, GetLastError());
        w(line);
        return NULL;
    }
    wsprintfA(line, "started %s (pid %lu)", exe, pi.dwProcessId);
    w(line);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
/* start exe and report whether it is still running 25 s later */
static int start_and_check(const char *exe, int minimized)
{
    DWORD code = 0;
    HANDLE p = start_exe(exe, minimized);
    if (!p) return 0;
    Sleep(25000);
    GetExitCodeProcess(p, &code);
    CloseHandle(p);
    wsprintfA(line, "%s %s after 25 s (exit code %lu)", exe,
              code == STILL_ACTIVE ? "still running" : "NOT running", code);
    w(line);
    return code == STILL_ACTIVE;
}
void __stdcall start(void)
{
    char mode[16], target[MAX_PATH];
    const char *c = GetCommandLineA();
    HANDLE m;
    DWORD t0;
    int i, n, take, seen = 0;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    out = CreateFileA(DIR "HANDOFF.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) ExitProcess(3);
    w("handoff9x 1.0");

    /* skip the program name, then argv[1] = mode, rest of the line = exe */
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; }
    else while (*c && *c != ' ' && *c != '\t') c++;
    while (*c == ' ' || *c == '\t') c++;
    for (n = 0; *c && *c != ' ' && *c != '\t' && n < (int)sizeof(mode) - 1; n++) mode[n] = *c++;
    mode[n] = '\0';
    while (*c == ' ' || *c == '\t') c++;
    for (n = 0; c[n] && c[n] != '\r' && c[n] != '\n' && n < MAX_PATH - 1; n++) target[n] = c[n];
    while (n > 0 && (target[n - 1] == ' ' || target[n - 1] == '\t')) n--;
    target[n] = '\0';
    take = !lstrcmpiA(mode, "take");
    if ((!take && lstrcmpiA(mode, "watch")) || !target[0]
        || GetFileAttributesA(target) == 0xFFFFFFFF) {
        wsprintfA(line, "usage: handoff9x take|watch <exe> - got \"%s\" \"%s\" (missing?)"
                  " - nothing done", mode, target);
        w(line);
        done(2);
    }
    wsprintfA(line, "%s %s: waiting for the agent's mutex to free - send RESTART now",
              take ? "take" : "watch", target);
    w(line);

    /* 1. held first (an agent launched us), then free */
    t0 = GetTickCount();
    for (i = 0; i < 3000; i++) {                  /* 3000 x 100 ms = 300 s */
        if (mutex_held()) {
            if (!seen) { seen = 1; w("mutex seen held by the running agent"); }
        } else if (seen) {
            break;
        } else if (i >= 50) {
            w("the agent's mutex was never seen held in 5 s - is an agent running? nothing done");
            done(6);
        }
        Sleep(100);
    }
    if (i == 3000) {
        w("the agent's mutex never freed in 300 s - the old agent still holds it; nothing done");
        done(4);
    }
    wsprintfA(line, "mutex freed after %lu ms", GetTickCount() - t0);
    w(line);

    if (!take) {
        /* 2w. did RESTART's relaunch come back? */
        for (i = 0; i < 900; i++) {               /* 900 x 100 ms = 90 s */
            if (mutex_held()) {
                wsprintfA(line, "an agent holds the mutex again after %lu ms - nothing to do",
                          GetTickCount() - t0);
                w(line);
                done(0);
            }
            Sleep(100);
        }
        w("NO agent came back within 90 s of the restart - starting the target");
        done(start_and_check(target, 0) ? 1 : 7);
    }

    /* 2t. take it before RESTART's relaunch does */
    SetLastError(0);
    m = CreateMutexA(NULL, FALSE, AGENT_MUTEX);
    if (!m || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (m) CloseHandle(m);
        w("lost the race: another agent took the mutex first - it keeps running, nothing done");
        done(5);
    }
    wsprintfA(line, "mutex TAKEN after %lu ms - RESTART's relaunch will exit", GetTickCount() - t0);
    w(line);
    Sleep(20000);
    CloseHandle(m);
    w("mutex released - starting the target minimized");
    if (start_and_check(target, 1)) {
        w("done - the target is the running agent");
        done(0);
    }
    w("fallback: starting C:\\RETRO_AGENT\\retro_agent.exe");
    start_and_check(DIR "retro_agent.exe", 0);
    done(1);
}
