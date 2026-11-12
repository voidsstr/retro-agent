/*
 * ai.c - Fleet AI transport: AI_HELLO / MODEL_LOAD / MODEL_LIST /
 * MODEL_UNLOAD / INFER_RUN / TENSOR / AI_RESTART.
 *
 * The agent does NOT run inference in-process. All AI frames are proxied to
 * a supervised retro-infer.exe --serve child on 127.0.0.1:9896 (the agent
 * itself owns 9897/9898). Crash isolation: a wedged GPU backend kills the
 * child, never the agent; the next AI command respawns it.
 *
 * retro-infer's reply frames use the same [len][status+data] shape as the
 * agent protocol, so replies are forwarded verbatim.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "../shared/aigate.h"
#include <stdio.h>
#include <string.h>

#define INFER_PORT 9896
#define INFER_SPAWN_WAIT_MS 400
#define INFER_SPAWN_TRIES 8

/* Opt-in gate (2026-07-23), hard-locked 2026-11-08 (1.99.0). The AI engine
 * (retro-infer.exe) must NOT run by default - on the single-core vintage fleet
 * boxes it steals cycles from games/benchmarks. Since 1.99.0 the opt-in flag
 * is no longer sufficient on its own: the box must ALSO be allowed, and
 * nothing in the agent ever writes that permission. Decisions and the full
 * rationale live in agent/shared/aigate.h. */
#define AI_ENABLE_KEY   "Software\\RetroAgent"
#define AI_ENABLE_VALUE "AIEngine"
#define AI_ALLOW_VALUE  "AIEngineAllowed"

static SOCKET g_infer_sock = INVALID_SOCKET;

/* Read one HKLM\Software\RetroAgent DWORD. A value that is absent, or that
 * is present with the WRONG TYPE, reads as not present - a REG_SZ "1" is not
 * a permission. */
static ai_regflag_t ai_read_flag(const char *name)
{
    ai_regflag_t f;
    HKEY h;
    DWORD type = 0, val = 0, size = sizeof(val);

    f.present = 0;
    f.value = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, AI_ENABLE_KEY, 0, KEY_QUERY_VALUE, &h)
            == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, name, NULL, &type, (BYTE *)&val, &size)
                == ERROR_SUCCESS && type == REG_DWORD) {
            f.present = 1;
            f.value = (unsigned long)val;
        }
        RegCloseKey(h);
    }
    return f;
}

/* Is this box ALLOWED to run the AI engine at all? Absent/0 => no. Shipped
 * default, so a freshly imaged or freshly updated box is locked with nobody
 * having to set anything. NOTHING in the agent writes this value. */
static int ai_engine_allowed(void)
{
    return ai_gate_allowed(ai_read_flag(AI_ALLOW_VALUE));
}

/* Is the AI engine runnable right now? Requires BOTH the permission and the
 * opt-in flag. Callers (infer_ensure, the status thread) use this one. */
int ai_engine_enabled(void)
{
    return ai_gate_may_run(ai_read_flag(AI_ALLOW_VALUE),
                           ai_read_flag(AI_ENABLE_VALUE));
}

/* Why the engine is in the state it is - one stable line for the log, the
 * console and the AI_ENABLE error text. */
static const char *ai_engine_why(void)
{
    return ai_gate_why(ai_read_flag(AI_ALLOW_VALUE),
                       ai_read_flag(AI_ENABLE_VALUE));
}

/* Record what this start's AI pass decided, so the post-condition can be READ
 * off the box (REGREAD HKLM Software\RetroAgent) instead of inferred from a
 * log that rotates. Same convention as PostSkipBoot / GameResBoot /
 * MonitorPowerBoot. */
#define AI_BOOT_VALUE "AIEngineBoot"

static void ai_engine_note_boot(const char *what)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, AI_ENABLE_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &h, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(h, AI_BOOT_VALUE, 0, REG_SZ, (const BYTE *)what,
                       (DWORD)strlen(what) + 1);
        RegCloseKey(h);
    }
}

/* Persist the enable flag. */
static void ai_engine_set_flag(int on)
{
    HKEY h;
    DWORD val = on ? 1 : 0;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, AI_ENABLE_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &h, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(h, AI_ENABLE_VALUE, 0, REG_DWORD,
                       (const BYTE *)&val, sizeof(val));
        RegCloseKey(h);
    }
}

/* The agent is threaded per client (main.c); the engine socket is shared
 * state, so AI commands serialize through one critical section. */
static CRITICAL_SECTION g_ai_cs;
static volatile LONG g_ai_cs_state = 0;   /* 0=uninit 1=initing 2=ready */

static void ai_lock(void)
{
    LONG s = InterlockedCompareExchange(&g_ai_cs_state, 1, 0);
    if (s == 0) {
        InitializeCriticalSection(&g_ai_cs);
        InterlockedExchange(&g_ai_cs_state, 2);
    } else {
        while (g_ai_cs_state != 2)
            Sleep(1);
    }
    EnterCriticalSection(&g_ai_cs);
}

static void ai_unlock(void)
{
    LeaveCriticalSection(&g_ai_cs);
}

static void infer_disconnect(void)
{
    if (g_infer_sock != INVALID_SOCKET) {
        closesocket(g_infer_sock);
        g_infer_sock = INVALID_SOCKET;
    }
}

/* Directory of the running agent exe, with trailing backslash */
static void agent_dir(char *buf, int cap)
{
    char *p;
    GetModuleFileNameA(NULL, buf, (DWORD)cap);
    p = strrchr(buf, '\\');
    if (p)
        p[1] = '\0';
}

/* Share path to the engine binary (registry-overridable, same convention as
 * the agent/chat auto-update in autoupdate.c). */
#define ENGINE_SHARE_DEFAULT \
    "\\\\192.168.1.122\\files\\Utility\\Retro Automation\\retro-infer.exe"

static DWORD file_size_of(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        return 0;
    return fad.nFileSizeLow;
}

/* Ensure retro-infer.exe is present next to the agent, pulling it from the
 * share if missing or a different size. Fixes "AI engine not available -
 * files not staged": a box that auto-updated the agent from the share but
 * never had the engine binary staged. Best-effort + quick; returns 1 if the
 * engine exists locally afterwards. */
static int stage_engine_from_share(void)
{
    char local[MAX_PATH + 32];
    char share[512];
    DWORD lsz, rsz;
    HKEY hk;

    /* Never pull the engine onto a box that is not allowed to run it. The
     * callers already check, but this is the one function that puts the
     * binary on disk, so it refuses on its own account too. */
    if (!ai_engine_allowed()) {
        log_msg(LOG_FILE, "AI: not staging the engine - %s", ai_engine_why());
        return 0;
    }

    agent_dir(local, MAX_PATH);
    strcat(local, "retro-infer.exe");

    /* registry override HKLM\Software\RetroAgent\EnginePath, else default */
    share[0] = '\0';
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0,
                      KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        DWORD n = sizeof(share), ty = REG_SZ;
        RegQueryValueExA(hk, "EnginePath", NULL, &ty,
                         (LPBYTE)share, &n);
        RegCloseKey(hk);
    }
    if (!share[0]) {
        strncpy(share, ENGINE_SHARE_DEFAULT, sizeof(share) - 1);
        share[sizeof(share) - 1] = '\0';
    }

    lsz = file_size_of(local);
    rsz = file_size_of(share);

    if (rsz == 0) {
        /* share unreachable — nothing we can do; caller reports status */
        log_msg(LOG_FILE, "AI: engine share not reachable (%s)", share);
        return lsz != 0;
    }
    if (lsz == rsz)
        return 1;   /* already staged + current */

    log_msg(LOG_FILE, "AI: staging engine from share (local=%lu remote=%lu)",
            (unsigned long)lsz, (unsigned long)rsz);

    /* stop a running engine so the file can be overwritten */
    if (g_infer_sock != INVALID_SOCKET) {
        frame_send(g_infer_sock, "SHUTDOWN", 8);
        infer_disconnect();
    }
    {
        /* taskkill any stray engine (best-effort; ignore result) */
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[] = "taskkill /f /im retro-infer.exe";
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                           NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        Sleep(300);
    }

    if (!CopyFileA(share, local, FALSE)) {
        log_msg(LOG_FILE, "AI: engine CopyFile failed: %lu",
                (unsigned long)GetLastError());
        return lsz != 0;   /* keep whatever we had */
    }
    log_msg(LOG_FILE, "AI: engine staged (%lu bytes)", (unsigned long)rsz);
    return 1;
}

/* Best-effort kill of any retro-infer.exe, however it was started (a stray
 * instance from an earlier enable no longer answers its socket). Ignores the
 * result: there is usually nothing to kill, and that is the healthy case. */
static void ai_taskkill_engine(void)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[] = "taskkill /f /im retro-infer.exe";
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* Undo a previous enable: stop the engine, clear the AIEngine flag and DELETE
 * the staged retro-infer.exe. Called at every start while the lock is in
 * force, and by AI_DISABLE.
 *
 * It reports what it ACTUALLY removed, not what it meant to - `detail` is
 * written for the log and for AI_DISABLE's reply, and a failed delete says so
 * with its error code rather than being folded into a reassuring summary.
 * Returns the number of things removed (0 on a box that was already clean,
 * which is what a settled box must read). */
static int ai_engine_purge(char *detail, int cap)
{
    char exe[MAX_PATH + 32];
    HKEY h;
    int removed = 0;
    int flag_cleared = 0, exe_deleted = 0;
    DWORD exe_err = 0;
    int had_exe = 0;

    if (g_infer_sock != INVALID_SOCKET) {
        frame_send(g_infer_sock, "SHUTDOWN", 8);
        infer_disconnect();
    }
    ai_taskkill_engine();

    /* the opt-in flag: delete it outright rather than writing 0, so the
     * steady state is "absent" - the shipped default */
    if (ai_read_flag(AI_ENABLE_VALUE).present) {
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, AI_ENABLE_KEY, 0, KEY_SET_VALUE,
                          &h) == ERROR_SUCCESS) {
            if (RegDeleteValueA(h, AI_ENABLE_VALUE) == ERROR_SUCCESS) {
                flag_cleared = 1;
                removed++;
            }
            RegCloseKey(h);
        }
    }

    /* the binary: this is what makes re-enabling instant, so it goes */
    agent_dir(exe, MAX_PATH);
    strcat(exe, "retro-infer.exe");
    if (GetFileAttributesA(exe) != 0xFFFFFFFF) {
        had_exe = 1;
        SetFileAttributesA(exe, FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileA(exe)) {
            exe_deleted = 1;
            removed++;
        } else {
            exe_err = GetLastError();
        }
    }

    if (detail && cap > 0) {
        _snprintf(detail, cap,
                  "AIEngine flag %s, retro-infer.exe %s%s",
                  flag_cleared ? "cleared" : "already absent",
                  exe_deleted ? "deleted" :
                      (had_exe ? "DELETE FAILED" : "not present"),
                  had_exe && !exe_deleted ? " (see error below)" : "");
        detail[cap - 1] = '\0';
    }
    if (had_exe && !exe_deleted)
        log_msg(LOG_FILE, "AI: could NOT delete %s: error %lu", exe,
                (unsigned long)exe_err);
    return removed;
}

static int infer_try_connect(void)
{
    struct sockaddr_in addr;
    SOCKET s;
    int one = 1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return 1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001UL);
    addr.sin_port = htons(INFER_PORT);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return 1;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    g_infer_sock = s;
    return 0;
}

static int infer_spawn(void)
{
    char exe[MAX_PATH + 64];
    char cmdline[MAX_PATH + 96];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    agent_dir(exe, MAX_PATH);
    strcat(exe, "retro-infer.exe");
    if (GetFileAttributesA(exe) == 0xFFFFFFFF) {
        /* not staged — try pulling it from the share before giving up */
        stage_engine_from_share();
        if (GetFileAttributesA(exe) == 0xFFFFFFFF) {
            log_msg(LOG_FILE, "AI: %s not found (share staging failed)", exe);
            return 1;
        }
    }
    _snprintf(cmdline, sizeof(cmdline), "\"%s\" --serve %d", exe, INFER_PORT);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        log_msg(LOG_FILE, "AI: CreateProcess failed %lu",
                (unsigned long)GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log_msg(LOG_FILE, "AI: spawned retro-infer --serve %d", INFER_PORT);
    return 0;
}

/* Get a live connection to the engine, spawning it if needed. */
static int infer_ensure(void)
{
    int i;
    if (g_infer_sock != INVALID_SOCKET)
        return 0;
    if (infer_try_connect() == 0)
        return 0;
    /* Opt-in gate: never auto-spawn the engine unless explicitly enabled. */
    if (!ai_engine_enabled()) {
        log_msg(LOG_FILE, "AI: engine disabled (enable via AI_ENABLE) - not spawning");
        return 1;
    }
    if (infer_spawn() != 0)
        return 1;
    for (i = 0; i < INFER_SPAWN_TRIES; i++) {
        Sleep(INFER_SPAWN_WAIT_MS);
        if (infer_try_connect() == 0)
            return 0;
    }
    return 1;
}

/* Round-trip under the AI lock. Returns 0 and the raw reply (caller frees)
 * on success. */
static int infer_roundtrip(const char *cmd, const char *payload,
                           DWORD payload_len, char **reply_out,
                           DWORD *reply_len_out)
{
    int attempt;
    ai_lock();
    for (attempt = 0; attempt < 2; attempt++) {
        char *reply = NULL;
        DWORD reply_len = 0;
        if (infer_ensure() != 0)
            break;
        if (frame_send(g_infer_sock, cmd, (DWORD)strlen(cmd)) != 0 ||
            (payload &&
             frame_send(g_infer_sock, payload, payload_len) != 0) ||
            frame_recv(g_infer_sock, &reply, &reply_len) != 0) {
            infer_disconnect();
            continue;   /* respawn + retry once */
        }
        ai_unlock();
        *reply_out = reply;
        *reply_len_out = reply_len;
        return 0;
    }
    ai_unlock();
    return 1;
}

/* Round-trip: send cmd (+ optional payload frame), forward reply verbatim. */
static void infer_proxy(SOCKET sock, const char *cmd,
                        const char *payload, DWORD payload_len)
{
    char *reply = NULL;
    DWORD reply_len = 0;
    /* Say WHY. "AI engine unavailable" is true of a locked box, a box with no
     * binary staged and a crashed engine alike, and they need different
     * answers from the operator. */
    if (!ai_engine_enabled()) {
        send_error_response(sock, ai_engine_why());
        return;
    }
    if (infer_roundtrip(cmd, payload, payload_len, &reply, &reply_len) != 0) {
        send_error_response(sock, "AI engine unavailable");
        return;
    }
    frame_send(sock, reply, reply_len);
    HeapFree(GetProcessHeap(), 0, reply);
}

/* ---- host GPU detection + driver advice (for AI_HELLO augmentation) ---- */

static void host_gpu_json(char *buf, int cap)
{
    DISPLAY_DEVICEA dd;
    char name[128] = "", devid[128] = "";
    int is_3dfx = 0, is_nvidia = 0, is_ati = 0, is_intel = 0;
    int glide_ok = 0, gl_ok = 0;
    HMODULE h;
    const char *flag;

    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesA(NULL, 0, &dd, 0)) {
        safe_strncpy(name, dd.DeviceString, sizeof(name));
        safe_strncpy(devid, dd.DeviceID, sizeof(devid));
    }
    is_3dfx = strstr(devid, "VEN_121A") != NULL ||
              strstr(name, "3dfx") != NULL || strstr(name, "Voodoo") != NULL;
    is_nvidia = strstr(devid, "VEN_10DE") != NULL ||
                strstr(name, "NVIDIA") != NULL ||
                strstr(name, "GeForce") != NULL;
    is_ati = strstr(devid, "VEN_1002") != NULL ||
             strstr(name, "ATI") != NULL || strstr(name, "Radeon") != NULL ||
             strstr(name, "AMD") != NULL;
    is_intel = strstr(devid, "VEN_8086") != NULL ||
               strstr(name, "Intel") != NULL;
    h = LoadLibraryA("glide3x.dll");
    if (h) {
        glide_ok = 1;
        FreeLibrary(h);
    }
    h = LoadLibraryA("opengl32.dll");
    if (h) {
        gl_ok = 1;
        FreeLibrary(h);
    }

    /* nv-gl (retro-infer's portable OpenGL 1.1 + ARB_multitexture binary-
     * GEMM backend, despite the name) is hardware-verified exact on
     * GeForce/Radeon/Intel-class cards — see
     * docs/machines/ai-capability-profiles.md. It only needs opengl32.dll
     * to load; the DLL-presence check here is the same shallow signal
     * glide_ok uses for glide-mac (the engine's own AI_HELLO does the
     * real ARB_multitexture capability check at init time). */
    if (is_3dfx && !glide_ok)
        flag = "3dfx GPU present but glide3x.dll is NOT loadable - stage "
               "glide3x.dll next to the agent to enable the glide-mac AI "
               "backend";
    else if (is_3dfx && glide_ok)
        flag = "ok: 3dfx GPU with loadable glide3x (glide-mac available)";
    else if ((is_nvidia || is_ati || is_intel) && gl_ok)
        flag = "ok: GPU with loadable OpenGL (nv-gl backend available, "
               "hardware-verified on NVIDIA/Radeon/Intel)";
    else if (is_nvidia || is_ati || is_intel)
        flag = "GPU present but opengl32.dll is NOT loadable - CPU "
               "backends only";
    else
        flag = "no known AI-capable GPU detected - CPU backends only";

    _snprintf(buf, cap,
              ",\"host_gpu\":{\"name\":\"%s\",\"is_3dfx\":%d,"
              "\"is_nvidia\":%d,\"is_ati\":%d,\"is_intel\":%d,"
              "\"glide3x_loadable\":%d,\"opengl_loadable\":%d,"
              "\"driver_flag\":\"%s\"}",
              name, is_3dfx, is_nvidia, is_ati, is_intel, glide_ok, gl_ok,
              flag);
    buf[cap - 1] = '\0';
}

void handle_ai_hello(SOCKET sock)
{
    char *reply = NULL;
    DWORD reply_len = 0;
    char gpu[512];
    char *merged;
    DWORD i, jend = 0;

    if (!ai_engine_enabled()) {
        send_error_response(sock, ai_engine_why());
        return;
    }
    if (infer_roundtrip("HELLO", NULL, 0, &reply, &reply_len) != 0) {
        send_error_response(sock, "AI engine unavailable");
        return;
    }
    /* splice host GPU info + driver advice before the JSON's final '}' */
    host_gpu_json(gpu, sizeof(gpu));
    for (i = reply_len; i > 1; i--) {
        if (reply[i - 1] == '}') {
            jend = i - 1;
            break;
        }
    }
    merged = (char *)HeapAlloc(GetProcessHeap(), 0,
                               reply_len + strlen(gpu) + 4);
    if (jend > 0 && merged) {
        memcpy(merged, reply, jend);
        memcpy(merged + jend, gpu, strlen(gpu));
        merged[jend + strlen(gpu)] = '}';
        frame_send(sock, merged, (DWORD)(jend + strlen(gpu) + 1));
    } else {
        frame_send(sock, reply, reply_len);
    }
    if (merged)
        HeapFree(GetProcessHeap(), 0, merged);
    HeapFree(GetProcessHeap(), 0, reply);
}

/* Startup status: probe (spawning if staged) and report AI readiness on the
 * agent console + log, so the box itself shows when it can take AI work. */
DWORD WINAPI ai_status_thread(LPVOID param)
{
    char *reply = NULL;
    DWORD reply_len = 0;
    char gpu[512];
    (void)param;
    Sleep(3000);   /* let listeners + shell settle */

    /* HARD LOCK (1.99.0). While this box is not allowed to run the engine we
     * do not merely decline to start it - we UNDO any earlier enable: stop a
     * stray instance, clear the AIEngine flag and delete the staged binary.
     * "Off by default" that leaves yesterday's enable and yesterday's
     * executable in place is not off.
     *
     * Deliberately NOT hostpolicy-guarded: the two boxes actually carrying a
     * staged retro-infer.exe on 2026-11-08 were .145 and .139, and .139 is a
     * modern Windows host. Deleting a file the agent staged into its own
     * directory is self-maintenance, not reconfiguring someone's PC.
     * tests/python/test_aigate_agent.py pins that both ways. */
    if (ai_gate_should_purge(ai_read_flag(AI_ALLOW_VALUE))) {
        char detail[256];
        int removed = ai_engine_purge(detail, sizeof(detail));
        char note[400];
        _snprintf(note, sizeof(note), "%s: %d removed (%s)",
                  ai_engine_why(), removed, detail);
        note[sizeof(note) - 1] = '\0';
        con_printf("AI: %s\n", note);
        log_msg(LOG_MAIN, "AI: %s", note);
        ai_engine_note_boot(note);
        return 0;
    }
    /* Allowed, but not switched on. */
    if (!ai_engine_enabled()) {
        con_printf("AI: %s\n", ai_engine_why());
        log_msg(LOG_MAIN, "AI: %s", ai_engine_why());
        ai_engine_note_boot(ai_engine_why());
        return 0;
    }
    /* self-heal: pull the engine binary from the share if it isn't staged */
    stage_engine_from_share();
    host_gpu_json(gpu, sizeof(gpu));
    if (infer_roundtrip("HELLO", NULL, 0, &reply, &reply_len) == 0) {
        /* reply[0] is the status byte; the rest is the caps JSON */
        con_printf("AI: READY for fleet AI requests\n");
        if (reply_len > 1)
            con_printf("AI: engine %.*s\n", (int)(reply_len - 1 > 300 ? 300 :
                                                  reply_len - 1), reply + 1);
        con_printf("AI: host%s\n", gpu + 1);   /* skip leading comma */
        log_msg(LOG_MAIN, "AI: engine ready%s", gpu);
        HeapFree(GetProcessHeap(), 0, reply);
    } else {
        con_printf("AI: engine NOT available (retro-infer.exe not staged next "
                   "to the agent?) - AI commands will fail\n");
        log_msg(LOG_MAIN, "AI: engine not available at startup");
    }
    return 0;
}

void handle_model_list(SOCKET sock)
{
    infer_proxy(sock, "LIST", NULL, 0);
}

void handle_model_unload(SOCKET sock, const char *args)
{
    char cmd[128];
    if (!args || !args[0]) {
        send_error_response(sock, "MODEL_UNLOAD requires a name");
        return;
    }
    _snprintf(cmd, sizeof(cmd), "UNLOAD %s", args);
    infer_proxy(sock, cmd, NULL, 0);
}

static int name_ok(const char *s)
{
    int i;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return i > 0 && i < 48;
}

/* MODEL_LOAD <name>: two-frame like UPLOAD; writes models\<name>.rim next to
 * the agent, then tells the engine to load it. */
void handle_model_load(SOCKET sock, const char *args)
{
    char *data = NULL;
    DWORD data_len = 0;
    char dir[MAX_PATH + 32], path[MAX_PATH + 96], cmd[MAX_PATH + 160];
    HANDLE hFile;
    DWORD written;

    if (!args || !name_ok(args)) {
        send_error_response(sock, "MODEL_LOAD requires a simple name");
        return;
    }
    if (frame_recv(sock, &data, &data_len) != 0) {
        send_error_response(sock, "Failed to receive model data");
        return;
    }
    agent_dir(dir, MAX_PATH);
    strcat(dir, "models");
    CreateDirectoryA(dir, NULL);
    _snprintf(path, sizeof(path), "%s\\%s.rim", dir, args);

    hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, data);
        send_error_response(sock, "Cannot create model file");
        return;
    }
    if (!WriteFile(hFile, data, data_len, &written, NULL) ||
        written != data_len) {
        CloseHandle(hFile);
        HeapFree(GetProcessHeap(), 0, data);
        send_error_response(sock, "Model write failed");
        return;
    }
    CloseHandle(hFile);
    HeapFree(GetProcessHeap(), 0, data);
    log_msg(LOG_FILE, "AI: MODEL_LOAD %s (%lu bytes)", args,
            (unsigned long)data_len);

    _snprintf(cmd, sizeof(cmd), "LOAD %s %s", args, path);
    infer_proxy(sock, cmd, NULL, 0);
}

/* INFER_RUN <name>: two-frame; payload = raw input, reply = f32 logits */
void handle_infer_run(SOCKET sock, const char *args)
{
    char *data = NULL;
    DWORD data_len = 0;
    char cmd[128];

    if (!args || !name_ok(args)) {
        send_error_response(sock, "INFER_RUN requires a model name");
        return;
    }
    if (frame_recv(sock, &data, &data_len) != 0) {
        send_error_response(sock, "Failed to receive input data");
        return;
    }
    _snprintf(cmd, sizeof(cmd), "INFER %s", args);
    infer_proxy(sock, cmd, data, data_len);
    HeapFree(GetProcessHeap(), 0, data);
}

/* TENSOR PUT <slot> (two-frame) | TENSOR GET <slot> | TENSOR DEL <slot> */
void handle_tensor(SOCKET sock, const char *args)
{
    char sub[8];
    const char *slot;
    char cmd[128];
    int i;

    if (!args) {
        send_error_response(sock, "TENSOR requires PUT|GET|DEL <slot>");
        return;
    }
    for (i = 0; args[i] && args[i] != ' ' && i < 7; i++)
        sub[i] = args[i];
    sub[i] = '\0';
    slot = args[i] == ' ' ? str_skip_spaces(args + i + 1) : "";
    if (!name_ok(slot)) {
        send_error_response(sock, "TENSOR: bad slot name");
        return;
    }
    if (_stricmp(sub, "PUT") == 0) {
        char *data = NULL;
        DWORD data_len = 0;
        if (frame_recv(sock, &data, &data_len) != 0) {
            send_error_response(sock, "Failed to receive tensor data");
            return;
        }
        _snprintf(cmd, sizeof(cmd), "TPUT %s", slot);
        infer_proxy(sock, cmd, data, data_len);
        HeapFree(GetProcessHeap(), 0, data);
    } else if (_stricmp(sub, "GET") == 0) {
        _snprintf(cmd, sizeof(cmd), "TGET %s", slot);
        infer_proxy(sock, cmd, NULL, 0);
    } else if (_stricmp(sub, "DEL") == 0) {
        _snprintf(cmd, sizeof(cmd), "TDEL %s", slot);
        infer_proxy(sock, cmd, NULL, 0);
    } else {
        send_error_response(sock, "TENSOR: unknown subcommand");
    }
}

/* AI_RAW <engine cmd...>: forward any serve command with no payload frame.
 * AI_RAWP <engine cmd...>: same but a payload frame follows (two-frame).
 * Generic pass-through so new engine verbs (NTINIT/NTSTEP/...) need no
 * agent release. */
void handle_ai_raw(SOCKET sock, const char *args)
{
    if (!args || !args[0]) {
        send_error_response(sock, "AI_RAW requires an engine command");
        return;
    }
    infer_proxy(sock, args, NULL, 0);
}

void handle_ai_rawp(SOCKET sock, const char *args)
{
    char *data = NULL;
    DWORD data_len = 0;
    if (!args || !args[0]) {
        send_error_response(sock, "AI_RAWP requires an engine command");
        return;
    }
    if (frame_recv(sock, &data, &data_len) != 0) {
        send_error_response(sock, "Failed to receive payload");
        return;
    }
    infer_proxy(sock, args, data, data_len);
    HeapFree(GetProcessHeap(), 0, data);
}

/* AI_RESTART: hard-restart the engine (hung GPU backend recovery) */
void handle_ai_restart(SOCKET sock)
{
    if (g_infer_sock != INVALID_SOCKET) {
        frame_send(g_infer_sock, "SHUTDOWN", 8);
        infer_disconnect();
    }
    /* also kill any wedged instance that no longer answers its socket */
    {
        char killcmd[] = "SHUTDOWN";
        (void)killcmd;
    }
    Sleep(300);
    if (infer_ensure() == 0)
        send_text_response(sock, "OK restarted");
    else
        send_error_response(sock, "AI engine did not come back");
}

/* AI_ENABLE: opt the AI engine in (persisted). Spawns it so it's ready. Issued
 * by the retro chat interaction; the engine is OFF until this is called. */
void handle_ai_enable(SOCKET sock)
{
    /* REFUSE, loudly. Setting the flag and answering OK on a locked box would
     * report success and change nothing - the exact shape this project keeps
     * paying for. The only way in is to set the permission by hand:
     *   REGWRITE HKLM Software\\RetroAgent AIEngineAllowed REG_DWORD 1
     * There is no command for it on purpose (cf. drvsafe.h / ALLOW3DFX). */
    if (!ai_gate_may_enable(ai_read_flag(AI_ALLOW_VALUE))) {
        char msg[320];
        _snprintf(msg, sizeof(msg),
                  "%s - refusing. The engine stays off and its binary stays "
                  "removed. To allow it on this box: REGWRITE HKLM "
                  "Software\\RetroAgent AIEngineAllowed REG_DWORD 1, then "
                  "AI_ENABLE again.", ai_engine_why());
        msg[sizeof(msg) - 1] = '\0';
        log_msg(LOG_FILE, "AI: AI_ENABLE refused - %s", ai_engine_why());
        send_error_response(sock, msg);
        return;
    }
    ai_engine_set_flag(1);
    log_msg(LOG_FILE, "AI: engine ENABLED via AI_ENABLE");
    if (infer_ensure() == 0)
        send_text_response(sock, "OK AI engine enabled and running");
    else
        send_text_response(sock, "OK AI engine enabled (will start on next "
                                 "AI request; binary may need staging)");
}

/* AI_DISABLE: opt the AI engine out (persisted) and stop any running instance.
 * The default state; call this to reclaim CPU on the vintage boxes. */
void handle_ai_disable(SOCKET sock)
{
    char detail[256];
    char msg[320];
    int removed;

    /* 1.99.0: disabling also REMOVES - the flag and the staged binary - so a
     * box that has been turned off cannot be turned back on in one command.
     * Reports the post-condition, not "OK". */
    removed = ai_engine_purge(detail, sizeof(detail));
    _snprintf(msg, sizeof(msg), "OK AI engine disabled and stopped; %d "
              "removed (%s)", removed, detail);
    msg[sizeof(msg) - 1] = '\0';
    log_msg(LOG_FILE, "AI: engine DISABLED via AI_DISABLE - %s", detail);
    send_text_response(sock, msg);
}
