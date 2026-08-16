#ifndef LOG_H
#define LOG_H

/*
 * log.h - Thread-safe verbose logging
 * Output to stderr + optional log file.
 * Format: [HH:MM:SS][TAG] message
 */

/* Initialize logger. File logging is ON BY DEFAULT and needs no argument:
 * logfile=NULL selects the ONE known location, C:\RETRO_AGENT\agent.log
 * (rotating, size-capped with one .1 backup), falling back to the exe's own
 * directory and then the temp dir if that is not writable. Starting the agent
 * by hand should never require remembering -l to get a log, and every fleet
 * tool should find one in the same place on every box. A non-empty logfile
 * still overrides the path. */
void log_init(const char *logfile);

/* The resolved active log-file path (for printing to the console). */
const char *log_path(void);

/* Log a message with tag and printf-style format. */
void log_msg(const char *tag, const char *fmt, ...);

/* Lock-free crash logger for use from the unhandled-exception filter — writes
 * straight to disk without taking the log lock (which a crashing thread may
 * already hold). Flushes anything still batched first, because what the agent
 * was doing before the crash is the useful half. Do not use on hot paths. */
void log_crash(const char *tag, const char *fmt, ...);

/*
 * Batched writes. Startup is deliberately unbuffered so a silent early crash
 * still leaves its last line on disk; call log_set_buffered(1) once the agent
 * is up and lines will accumulate in memory, written out when the buffer
 * fills or the flush interval passes.
 *
 * Motivation: per-line FlushFileBuffers means a physical seek and platter
 * write for every log entry on a machine that runs 24/7 — hard on the Win98
 * box's 1997 IDE drive.
 */
void log_set_buffered(int on);

/* Force pending lines to disk now. Call before anything that may end the
 * process or the machine (reboot, update, shutdown) — and it is what the
 * console-close handler and the crash filter use. */
void log_flush(void);

/* Flush, stop the flusher thread, close the file. Clean-exit path. */
void log_shutdown(void);

/* Standard tags */
#define LOG_MAIN  "MAIN"
#define LOG_NET   "NET"
#define LOG_FILE  "FILE"
#define LOG_REG   "REG"
#define LOG_EXEC  "EXEC"
#define LOG_VIDEO "VIDEO"
#define LOG_PROTO "PROTO"

#endif /* LOG_H */
