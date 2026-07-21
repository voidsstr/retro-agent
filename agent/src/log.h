#ifndef LOG_H
#define LOG_H

/*
 * log.h - Thread-safe verbose logging
 * Output to stderr + optional log file.
 * Format: [HH:MM:SS][TAG] message
 */

/* Initialize logger. File logging is ON BY DEFAULT: logfile=NULL selects the
 * default rotating file (<exe dir>\agent.log, size-capped with one .1 backup);
 * a non-empty logfile overrides the path. */
void log_init(const char *logfile);

/* The resolved active log-file path (for printing to the console). */
const char *log_path(void);

/* Log a message with tag and printf-style format. */
void log_msg(const char *tag, const char *fmt, ...);

/* Lock-free crash logger for use from the unhandled-exception filter — writes
 * straight to disk without taking the log lock (which a crashing thread may
 * already hold). Do not use on hot paths. */
void log_crash(const char *tag, const char *fmt, ...);

/* Standard tags */
#define LOG_MAIN  "MAIN"
#define LOG_NET   "NET"
#define LOG_FILE  "FILE"
#define LOG_REG   "REG"
#define LOG_EXEC  "EXEC"
#define LOG_VIDEO "VIDEO"
#define LOG_PROTO "PROTO"

#endif /* LOG_H */
