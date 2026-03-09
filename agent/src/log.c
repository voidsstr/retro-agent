/*
 * log.c - Thread-safe verbose logging with timestamps
 * Uses CRITICAL_SECTION for thread safety, GetLocalTime for timestamps.
 * Outputs to stderr and optionally to a log file.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#include "log.h"

static CRITICAL_SECTION g_log_cs;
static FILE *g_log_file = NULL;
static int g_log_initialized = 0;

void log_init(const char *logfile)
{
    InitializeCriticalSection(&g_log_cs);
    g_log_initialized = 1;

    if (logfile && logfile[0]) {
        g_log_file = fopen(logfile, "a");
        if (!g_log_file) {
            fprintf(stderr, "[LOG] WARNING: Cannot open log file: %s\n", logfile);
        }
    }
}

void log_msg(const char *tag, const char *fmt, ...)
{
    SYSTEMTIME st;
    char prefix[64];
    char msg[2048];
    va_list ap;

    if (!g_log_initialized) return;

    GetLocalTime(&st);
    _snprintf(prefix, sizeof(prefix), "[%02u:%02u:%02u][%-5s] ",
              st.wHour, st.wMinute, st.wSecond, tag);
    prefix[sizeof(prefix) - 1] = '\0';

    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    EnterCriticalSection(&g_log_cs);

    fprintf(stderr, "%s%s\n", prefix, msg);
    fflush(stderr);

    if (g_log_file) {
        fprintf(g_log_file, "%s%s\n", prefix, msg);
        fflush(g_log_file);
    }

    LeaveCriticalSection(&g_log_cs);
}
