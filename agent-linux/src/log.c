/*
 * log.c - Thread-safe logging with timestamps.
 * Uses pthread_mutex for thread safety, localtime_r for timestamps.
 */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

#include "log.h"

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_file = NULL;
static int g_log_initialized = 0;

void log_init(const char *logfile)
{
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
    struct tm tm_buf;
    time_t now;
    char prefix[64];
    char msg[2048];
    va_list ap;

    if (!g_log_initialized) return;

    now = time(NULL);
    localtime_r(&now, &tm_buf);
    snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d][%-5s] ",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, tag);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    pthread_mutex_lock(&g_log_mutex);

    fprintf(stderr, "%s%s\n", prefix, msg);
    fflush(stderr);

    if (g_log_file) {
        fprintf(g_log_file, "%s%s\n", prefix, msg);
        fflush(g_log_file);
    }

    pthread_mutex_unlock(&g_log_mutex);
}
