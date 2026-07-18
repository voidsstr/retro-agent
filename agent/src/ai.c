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
#include <stdio.h>
#include <string.h>

#define INFER_PORT 9896
#define INFER_SPAWN_WAIT_MS 400
#define INFER_SPAWN_TRIES 8

static SOCKET g_infer_sock = INVALID_SOCKET;

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
               "hardware-verified on Radeon/Intel)";
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
    /* self-heal: pull the engine binary from the share if it isn't staged */
    stage_engine_from_share();
    host_gpu_json(gpu, sizeof(gpu));
    if (infer_roundtrip("HELLO", NULL, 0, &reply, &reply_len) == 0) {
        /* reply[0] is the status byte; the rest is the caps JSON */
        printf("AI: READY for fleet AI requests\n");
        if (reply_len > 1)
            printf("AI: engine %.*s\n", (int)(reply_len - 1 > 300 ? 300 :
                                              reply_len - 1), reply + 1);
        printf("AI: host%s\n", gpu + 1);   /* skip leading comma */
        log_msg(LOG_MAIN, "AI: engine ready%s", gpu);
        HeapFree(GetProcessHeap(), 0, reply);
    } else {
        printf("AI: engine NOT available (retro-infer.exe not staged next "
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
