/* agentswap9x - install C:\RETRO_AGENT\retro_agent_new.exe over a RUNNING agent
 * on Windows 9x (which cannot replace a running exe), with automatic rollback.
 *   1. wait (max 120 s) for RETRO_AGENT.EXE to exit - send the agent QUIT
 *   2. retro_agent.exe -> retro_agent_prev.exe, retro_agent_new.exe -> retro_agent.exe
 *   3. start it, and if it is not still running 25 s later, put the previous
 *      build back and start that instead - the box is never left without an agent.
 * No C runtime. Log: C:\RETRO_AGENT\AGENTSWAP.TXT */
#include <windows.h>
#include <tlhelp32.h>
#define DIR "C:\\RETRO_AGENT\\"
static HANDLE out; static char line[400];
static void w(const char *s) { DWORD n; WriteFile(out, s, lstrlenA(s), &n, NULL); WriteFile(out, "\r\n", 2, &n, NULL); FlushFileBuffers(out); }
static int agent_running(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); PROCESSENTRY32 pe; int found = 0;
    if (snap == INVALID_HANDLE_VALUE) return 1;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) do {
        const char *n = pe.szExeFile, *q; for (q = pe.szExeFile; *q; q++) if (*q == '\\') n = q + 1;
        if (!lstrcmpiA(n, "RETRO_AGENT.EXE")) found = 1;
    } while (!found && Process32Next(snap, &pe));
    CloseHandle(snap); return found;
}
static HANDLE start_agent(void) {
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si); ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(DIR "retro_agent.exe", NULL, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, DIR, &si, &pi)) {
        wsprintfA(line, "CreateProcess failed: %lu", GetLastError()); w(line); return NULL;
    }
    CloseHandle(pi.hThread); return pi.hProcess;
}
void __stdcall start(void) {
    int i; HANDLE p; DWORD code = 0;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    out = CreateFileA(DIR "AGENTSWAP.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) ExitProcess(3);
    w("agentswap9x 1.0: waiting for RETRO_AGENT.EXE to exit");
    if (GetFileAttributesA(DIR "retro_agent_new.exe") == 0xFFFFFFFF) { w("no retro_agent_new.exe - nothing to do"); ExitProcess(2); }
    for (i = 0; i < 240 && agent_running(); i++) Sleep(500);
    if (agent_running()) { w("agent still running after 120 s - NOT swapping"); CloseHandle(out); ExitProcess(4); }
    Sleep(1500);                                   /* let the file handle go */
    DeleteFileA(DIR "retro_agent_prev.exe");
    if (!MoveFileA(DIR "retro_agent.exe", DIR "retro_agent_prev.exe")) {
        wsprintfA(line, "could not move the old exe aside (%lu) - restarting it unchanged", GetLastError()); w(line);
        p = start_agent(); if (p) CloseHandle(p); CloseHandle(out); ExitProcess(5);
    }
    if (!MoveFileA(DIR "retro_agent_new.exe", DIR "retro_agent.exe")) {
        wsprintfA(line, "could not move the new exe in (%lu) - restoring", GetLastError()); w(line);
        MoveFileA(DIR "retro_agent_prev.exe", DIR "retro_agent.exe");
        p = start_agent(); if (p) CloseHandle(p); CloseHandle(out); ExitProcess(6);
    }
    w("swapped - starting the new build");
    p = start_agent();
    if (p) { Sleep(25000); GetExitCodeProcess(p, &code); }
    if (p && code == STILL_ACTIVE) { w("new build still running after 25 s - done"); CloseHandle(p); CloseHandle(out); ExitProcess(0); }
    wsprintfA(line, "new build NOT running (exit code %lu) - rolling back", code); w(line);
    if (p) CloseHandle(p);
    DeleteFileA(DIR "retro_agent_bad.exe");
    MoveFileA(DIR "retro_agent.exe", DIR "retro_agent_bad.exe");
    MoveFileA(DIR "retro_agent_prev.exe", DIR "retro_agent.exe");
    p = start_agent(); if (p) CloseHandle(p);
    w("previous build restored and started");
    CloseHandle(out); ExitProcess(1);
}
