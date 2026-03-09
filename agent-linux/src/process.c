/*
 * process.c - Process listing and management via /proc.
 * PROCLIST reads /proc/[pid] dirs, PROCKILL via kill().
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>

void handle_proclist(SOCKET sock)
{
    DIR *proc_dir;
    struct dirent *ent;
    json_t j;
    char *result;

    json_init(&j);
    json_array_start(&j);

    proc_dir = opendir("/proc");
    if (proc_dir) {
        while ((ent = readdir(proc_dir)) != NULL) {
            char stat_path[256], stat_line[1024];
            FILE *f;
            int pid;
            char name[256] = "";
            int ppid = 0, threads = 0;
            char state = '?';

            /* Only numeric directories = processes */
            if (!isdigit((unsigned char)ent->d_name[0]))
                continue;

            pid = atoi(ent->d_name);
            snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);

            f = fopen(stat_path, "r");
            if (!f) continue;

            if (fgets(stat_line, sizeof(stat_line), f)) {
                /* Parse: pid (comm) state ppid ... num_threads */
                char *open_paren = strchr(stat_line, '(');
                char *close_paren = strrchr(stat_line, ')');
                if (open_paren && close_paren && close_paren > open_paren) {
                    int name_len = (int)(close_paren - open_paren - 1);
                    if (name_len >= (int)sizeof(name)) name_len = sizeof(name) - 1;
                    memcpy(name, open_paren + 1, name_len);
                    name[name_len] = '\0';

                    /* Fields after close paren: state ppid pgrp session tty_nr tpgid ...
                       field 20 is num_threads */
                    sscanf(close_paren + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %d",
                           &state, &ppid, &threads);
                }
            }
            fclose(f);

            json_object_start(&j);
            json_kv_uint(&j, "pid", (unsigned long)pid);
            json_kv_str(&j, "name", name);
            json_kv_int(&j, "threads", threads);
            json_kv_uint(&j, "parent_pid", (unsigned long)ppid);
            {
                char state_str[2] = { state, '\0' };
                json_kv_str(&j, "state", state_str);
            }
            json_object_end(&j);
        }
        closedir(proc_dir);
    }

    json_array_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

void handle_prockill(SOCKET sock, const char *args)
{
    int pid;

    if (!args || !args[0]) {
        send_error_response(sock, "PROCKILL requires a PID");
        return;
    }

    pid = atoi(args);
    if (pid <= 0) {
        send_error_response(sock, "Invalid PID");
        return;
    }

    if (kill(pid, SIGTERM) == 0) {
        send_text_response(sock, "OK");
    } else {
        char err[128];
        snprintf(err, sizeof(err), "kill(%d) failed: %s", pid, strerror(errno));
        send_error_response(sock, err);
    }
}
