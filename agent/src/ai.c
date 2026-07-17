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
        log_msg(LOG_FILE, "AI: %s not found", exe);
        return 1;
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

/* Round-trip: send cmd (+ optional payload frame), forward reply verbatim.
 * Retries once through a respawn if the engine connection is dead. */
static void infer_proxy(SOCKET sock, const char *cmd,
                        const char *payload, DWORD payload_len)
{
    int attempt;
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
        frame_send(sock, reply, reply_len);
        HeapFree(GetProcessHeap(), 0, reply);
        return;
    }
    send_error_response(sock, "AI engine unavailable");
}

void handle_ai_hello(SOCKET sock)
{
    infer_proxy(sock, "HELLO", NULL, 0);
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
