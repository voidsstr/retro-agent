/*
 * exec.c - Remote command execution via fork/exec with pipe capture.
 * EXEC: blocking with stdout capture. LAUNCH: non-blocking, returns PID.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#define EXEC_BUF_SIZE    65536

void handle_exec(SOCKET sock, const char *args)
{
    int pipefd[2];
    pid_t pid;
    char *output = NULL;
    uint32_t output_len = 0;
    uint32_t output_cap = EXEC_BUF_SIZE;
    int status;

    if (!args || !args[0]) {
        send_error_response(sock, "EXEC requires a command");
        return;
    }

    log_msg(LOG_EXEC, "EXEC: cmd=\"%s\"", args);

    if (pipe(pipefd) < 0) {
        send_error_response(sock, "Failed to create pipe");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        send_error_response(sock, "fork() failed");
        return;
    }

    if (pid == 0) {
        /* Child: redirect stdout+stderr to pipe, exec shell */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execl("/bin/sh", "sh", "-c", args, (char *)NULL);
        _exit(127);
    }

    /* Parent: read output from pipe */
    close(pipefd[1]);

    output = (char *)malloc(output_cap);
    if (!output) {
        close(pipefd[0]);
        waitpid(pid, &status, 0);
        send_error_response(sock, "Out of memory");
        return;
    }

    while (1) {
        ssize_t n;
        if (output_len + 4096 > output_cap) {
            output_cap *= 2;
            output = (char *)realloc(output, output_cap);
            if (!output) break;
        }

        n = read(pipefd[0], output + output_len, 4096);
        if (n <= 0) break;
        output_len += (uint32_t)n;
    }

    close(pipefd[0]);
    waitpid(pid, &status, 0);

    log_msg(LOG_EXEC, "Process %d exited, status=%d, output=%u bytes",
            (int)pid, WEXITSTATUS(status), output_len);

    if (output) {
        output[output_len] = '\0';
        send_text_response(sock, output);
        free(output);
    } else {
        send_text_response(sock, "(no output)");
    }
}

void handle_launch(SOCKET sock, const char *args)
{
    pid_t pid;
    json_t j;

    if (!args || !args[0]) {
        send_error_response(sock, "LAUNCH requires a command");
        return;
    }

    log_msg(LOG_EXEC, "LAUNCH: cmd=\"%s\"", args);

    pid = fork();
    if (pid < 0) {
        send_error_response(sock, "fork() failed");
        return;
    }

    if (pid == 0) {
        /* Child: detach and exec */
        setsid();
        execl("/bin/sh", "sh", "-c", args, (char *)NULL);
        _exit(127);
    }

    /* Parent: return PID immediately */
    log_msg(LOG_EXEC, "LAUNCH OK, PID=%d", (int)pid);

    json_init(&j);
    json_object_start(&j);
    json_kv_uint(&j, "pid", (unsigned long)pid);
    json_kv_str(&j, "command", args);
    json_object_end(&j);

    {
        char *result = json_finish(&j);
        if (result) {
            send_text_response(sock, result);
        } else {
            send_error_response(sock, "Out of memory");
        }
    }
    json_free(&j);
}
