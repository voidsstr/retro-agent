/*
 * files.c - File transfer and directory operations via POSIX API.
 * Upload/download, directory listing, mkdir, delete, filecopy.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

void handle_upload(SOCKET sock, const char *args)
{
    char *data = NULL;
    uint32_t data_len = 0;
    FILE *f;

    if (!args || !args[0]) {
        send_error_response(sock, "UPLOAD requires a path");
        return;
    }

    /* Next frame contains the file data */
    if (frame_recv(sock, &data, &data_len) != 0) {
        send_error_response(sock, "Failed to receive file data");
        return;
    }

    f = fopen(args, "wb");
    if (!f) {
        log_msg(LOG_FILE, "UPLOAD: fopen(\"%s\") failed: %s", args, strerror(errno));
        free(data);
        char err[256];
        snprintf(err, sizeof(err), "Cannot create file: %s", strerror(errno));
        send_error_response(sock, err);
        return;
    }

    if (fwrite(data, 1, data_len, f) != data_len) {
        log_msg(LOG_FILE, "UPLOAD: fwrite failed: %s", strerror(errno));
        fclose(f);
        free(data);
        send_error_response(sock, "Write failed");
        return;
    }

    fclose(f);
    free(data);

    log_msg(LOG_FILE, "UPLOAD: Complete, %u bytes to \"%s\"", data_len, args);

    {
        char resp[64];
        snprintf(resp, sizeof(resp), "OK %u", data_len);
        send_text_response(sock, resp);
    }
}

void handle_download(SOCKET sock, const char *args)
{
    FILE *f;
    struct stat st;
    char *buf;
    size_t bytes_read;

    if (!args || !args[0]) {
        send_error_response(sock, "DOWNLOAD requires a path");
        return;
    }

    if (stat(args, &st) != 0) {
        char err[256];
        snprintf(err, sizeof(err), "Cannot stat file: %s", strerror(errno));
        send_error_response(sock, err);
        return;
    }

    if ((uint32_t)st.st_size > MAX_FRAME_SIZE) {
        send_error_response(sock, "File too large");
        return;
    }

    f = fopen(args, "rb");
    if (!f) {
        char err[256];
        snprintf(err, sizeof(err), "Cannot open file: %s", strerror(errno));
        send_error_response(sock, err);
        return;
    }

    buf = (char *)malloc(st.st_size);
    if (!buf) {
        fclose(f);
        send_error_response(sock, "Out of memory");
        return;
    }

    bytes_read = fread(buf, 1, st.st_size, f);
    fclose(f);

    send_binary_response(sock, buf, (uint32_t)bytes_read);
    free(buf);
}

void handle_dirlist(SOCKET sock, const char *args)
{
    DIR *dir;
    struct dirent *ent;
    json_t j;
    char *result;

    if (!args || !args[0]) {
        send_error_response(sock, "DIRLIST requires a path");
        return;
    }

    dir = opendir(args);
    if (!dir) {
        char err[256];
        snprintf(err, sizeof(err), "Cannot open directory: %s", strerror(errno));
        send_error_response(sock, err);
        return;
    }

    json_init(&j);
    json_array_start(&j);

    while ((ent = readdir(dir)) != NULL) {
        struct stat st;
        char fullpath[4096];

        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        snprintf(fullpath, sizeof(fullpath), "%s/%s", args, ent->d_name);

        json_object_start(&j);
        json_kv_str(&j, "name", ent->d_name);

        if (stat(fullpath, &st) == 0) {
            json_kv_bool(&j, "is_dir", S_ISDIR(st.st_mode));
            json_kv_uint(&j, "size", (unsigned long)st.st_size);

            {
                struct tm tm_buf;
                char date[32];
                localtime_r(&st.st_mtime, &tm_buf);
                snprintf(date, sizeof(date), "%04d-%02d-%02d %02d:%02d:%02d",
                         tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                         tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
                json_kv_str(&j, "modified", date);
            }

            json_kv_bool(&j, "hidden", ent->d_name[0] == '.');
            json_kv_bool(&j, "readonly", access(fullpath, W_OK) != 0);
        } else {
            json_kv_bool(&j, "is_dir", ent->d_type == DT_DIR);
            json_kv_uint(&j, "size", 0);
        }

        json_object_end(&j);
    }

    closedir(dir);

    json_array_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

static int mkdir_recursive(const char *path)
{
    char tmp[4096];
    char *p;

    safe_strncpy(tmp, path, sizeof(tmp));

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

void handle_mkdir(SOCKET sock, const char *args)
{
    if (!args || !args[0]) {
        send_error_response(sock, "MKDIR requires a path");
        return;
    }

    if (mkdir_recursive(args) == 0) {
        send_text_response(sock, "OK");
    } else {
        char err[256];
        snprintf(err, sizeof(err), "mkdir failed: %s", strerror(errno));
        send_error_response(sock, err);
    }
}

void handle_delete(SOCKET sock, const char *args)
{
    if (!args || !args[0]) {
        send_error_response(sock, "DELETE requires a path");
        return;
    }

    if (unlink(args) == 0) {
        send_text_response(sock, "OK");
    } else {
        char err[256];
        snprintf(err, sizeof(err), "delete failed: %s", strerror(errno));
        send_error_response(sock, err);
    }
}

void handle_filecopy(SOCKET sock, const char *args)
{
    char src[2048], dst[2048];
    FILE *fin, *fout;
    char buf[65536];
    size_t n;

    if (!args || !args[0]) {
        send_error_response(sock, "FILECOPY requires src dst");
        return;
    }

    /* Parse "src dst" */
    {
        const char *space = strchr(args, ' ');
        if (!space) {
            send_error_response(sock, "FILECOPY requires src dst");
            return;
        }
        int src_len = (int)(space - args);
        if (src_len >= (int)sizeof(src)) src_len = sizeof(src) - 1;
        memcpy(src, args, src_len);
        src[src_len] = '\0';
        safe_strncpy(dst, str_skip_spaces(space + 1), sizeof(dst));
    }

    fin = fopen(src, "rb");
    if (!fin) {
        char err[256];
        snprintf(err, sizeof(err), "Cannot open source: %s", strerror(errno));
        send_error_response(sock, err);
        return;
    }

    fout = fopen(dst, "wb");
    if (!fout) {
        char err[256];
        fclose(fin);
        snprintf(err, sizeof(err), "Cannot create destination: %s", strerror(errno));
        send_error_response(sock, err);
        return;
    }

    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, n, fout) != n) {
            fclose(fin);
            fclose(fout);
            send_error_response(sock, "Write failed during copy");
            return;
        }
    }

    fclose(fin);
    fclose(fout);
    send_text_response(sock, "OK");
}
