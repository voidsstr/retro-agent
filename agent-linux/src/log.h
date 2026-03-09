#ifndef LOG_H
#define LOG_H

/*
 * log.h - Thread-safe logging for Linux agent.
 * Output to stderr + optional log file.
 * Format: [HH:MM:SS][TAG] message
 */

/* Initialize logger. logfile=NULL for console only. */
void log_init(const char *logfile);

/* Log a message with tag and printf-style format. */
void log_msg(const char *tag, const char *fmt, ...);

/* Standard tags */
#define LOG_MAIN  "MAIN"
#define LOG_NET   "NET"
#define LOG_FILE  "FILE"
#define LOG_EXEC  "EXEC"
#define LOG_PROTO "PROTO"
#define LOG_PKG   "PKG"
#define LOG_SVC   "SVC"

#endif /* LOG_H */
