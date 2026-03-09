/*
 * files.c - File transfer and directory operations
 * Upload/download, directory listing, mkdir, delete
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

void handle_upload(SOCKET sock, const char *args)
{
    char *data = NULL;
    DWORD data_len = 0;
    HANDLE hFile;

    if (!args || !args[0]) {
        send_error_response(sock, "UPLOAD requires a path");
        return;
    }

    /* Next frame contains the file data */
    if (frame_recv(sock, &data, &data_len) != 0) {
        send_error_response(sock, "Failed to receive file data");
        return;
    }

    hFile = CreateFileA(args, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char err[256];
        DWORD gle = GetLastError();
        log_msg(LOG_FILE, "UPLOAD: CreateFileA(\"%s\") failed, error %lu", args, (unsigned long)gle);
        _snprintf(err, sizeof(err), "Cannot create file: error %lu", gle);
        HeapFree(GetProcessHeap(), 0, data);
        send_error_response(sock, err);
        return;
    }
    log_msg(LOG_FILE, "UPLOAD: CreateFileA(\"%s\") = OK", args);

    {
        DWORD written;
        DWORD total = 0;
        while (total < data_len) {
            DWORD chunk = data_len - total;
            if (chunk > 65536) chunk = 65536;
            if (!WriteFile(hFile, data + total, chunk, &written, NULL)) {
                log_msg(LOG_FILE, "UPLOAD: WriteFile failed at offset %lu", (unsigned long)total);
                CloseHandle(hFile);
                HeapFree(GetProcessHeap(), 0, data);
                send_error_response(sock, "Write failed");
                return;
            }
            log_msg(LOG_FILE, "UPLOAD: WriteFile %lu bytes at offset %lu",
                    (unsigned long)written, (unsigned long)total);
            total += written;
        }
    }

    CloseHandle(hFile);
    HeapFree(GetProcessHeap(), 0, data);

    log_msg(LOG_FILE, "UPLOAD: Complete, %lu bytes to \"%s\"",
            (unsigned long)data_len, args);

    {
        char resp[64];
        _snprintf(resp, sizeof(resp), "OK %lu", (unsigned long)data_len);
        send_text_response(sock, resp);
    }
}

void handle_download(SOCKET sock, const char *args)
{
    HANDLE hFile;
    DWORD file_size, bytes_read;
    char *buf;

    if (!args || !args[0]) {
        send_error_response(sock, "DOWNLOAD requires a path");
        return;
    }

    hFile = CreateFileA(args, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char err[256];
        DWORD gle = GetLastError();
        log_msg(LOG_FILE, "DOWNLOAD: CreateFileA(\"%s\") failed, error %lu", args, (unsigned long)gle);
        _snprintf(err, sizeof(err), "Cannot open file: error %lu", gle);
        send_error_response(sock, err);
        return;
    }
    log_msg(LOG_FILE, "DOWNLOAD: CreateFileA(\"%s\") = OK", args);

    file_size = GetFileSize(hFile, NULL);
    if (file_size == INVALID_FILE_SIZE || file_size > MAX_FRAME_SIZE) {
        log_msg(LOG_FILE, "DOWNLOAD: GetFileSize failed or too large (%lu)", (unsigned long)file_size);
        CloseHandle(hFile);
        send_error_response(sock, "File too large or error getting size");
        return;
    }
    log_msg(LOG_FILE, "DOWNLOAD: GetFileSize = %lu bytes", (unsigned long)file_size);

    buf = (char *)HeapAlloc(GetProcessHeap(), 0, file_size);
    if (!buf) {
        CloseHandle(hFile);
        send_error_response(sock, "Out of memory");
        return;
    }

    {
        DWORD total = 0;
        while (total < file_size) {
            if (!ReadFile(hFile, buf + total, file_size - total,
                          &bytes_read, NULL) || bytes_read == 0) {
                break;
            }
            total += bytes_read;
        }
        file_size = total;
    }

    CloseHandle(hFile);
    send_binary_response(sock, buf, file_size);
    HeapFree(GetProcessHeap(), 0, buf);
}

void handle_dirlist(SOCKET sock, const char *args)
{
    WIN32_FIND_DATAA fd;
    HANDLE hFind;
    json_t j;
    char *result;
    char pattern[MAX_PATH];

    if (!args || !args[0]) {
        send_error_response(sock, "DIRLIST requires a path");
        return;
    }

    /* Append \* if path doesn't end with a wildcard */
    safe_strncpy(pattern, args, sizeof(pattern));
    {
        int len = (int)strlen(pattern);
        if (len > 0 && pattern[len - 1] != '*') {
            if (pattern[len - 1] != '\\' && pattern[len - 1] != '/')
                strncat(pattern, "\\", sizeof(pattern) - len - 1);
            strncat(pattern, "*", sizeof(pattern) - strlen(pattern) - 1);
        }
    }

    json_init(&j);
    json_array_start(&j);

    log_msg(LOG_FILE, "DIRLIST: FindFirstFileA(\"%s\")", pattern);

    hFind = FindFirstFileA(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            SYSTEMTIME st;
            char date[32];

            /* Skip . and .. */
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0)
                continue;

            log_msg(LOG_FILE, "DIRLIST: entry: %s (%lu bytes%s)",
                    fd.cFileName, (unsigned long)fd.nFileSizeLow,
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? ", dir" : "");

            json_object_start(&j);
            json_kv_str(&j, "name", fd.cFileName);
            json_kv_bool(&j, "is_dir",
                         (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
            json_kv_uint(&j, "size", fd.nFileSizeLow);

            if (FileTimeToSystemTime(&fd.ftLastWriteTime, &st)) {
                _snprintf(date, sizeof(date), "%04u-%02u-%02u %02u:%02u:%02u",
                          st.wYear, st.wMonth, st.wDay,
                          st.wHour, st.wMinute, st.wSecond);
                json_kv_str(&j, "modified", date);
            }

            json_kv_bool(&j, "hidden",
                         (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0);
            json_kv_bool(&j, "readonly",
                         (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0);
            json_object_end(&j);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    json_array_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

static BOOL create_directory_recursive(const char *path)
{
    char tmp[MAX_PATH];
    char *p;

    safe_strncpy(tmp, path, sizeof(tmp));

    for (p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            log_msg(LOG_FILE, "MKDIR: CreateDirectoryA(\"%s\") = %lu", tmp, GetLastError());
            *p = '\\';
        }
    }
    {
        BOOL ok = CreateDirectoryA(tmp, NULL) ||
                  GetLastError() == ERROR_ALREADY_EXISTS;
        log_msg(LOG_FILE, "MKDIR: CreateDirectoryA(\"%s\") final = %s", tmp, ok ? "OK" : "FAIL");
        return ok;
    }
}

void handle_mkdir(SOCKET sock, const char *args)
{
    if (!args || !args[0]) {
        send_error_response(sock, "MKDIR requires a path");
        return;
    }

    if (create_directory_recursive(args)) {
        send_text_response(sock, "OK");
    } else {
        char err[256];
        _snprintf(err, sizeof(err), "mkdir failed: error %lu", GetLastError());
        send_error_response(sock, err);
    }
}

/* Recursively delete a directory tree. Returns TRUE on success. */
static BOOL delete_tree(const char *path)
{
    WIN32_FIND_DATAA fd;
    HANDLE hFind;
    char pattern[MAX_PATH];
    char child[MAX_PATH];

    _snprintf(pattern, sizeof(pattern), "%s\\*", path);
    hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return RemoveDirectoryA(path);

    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;

        _snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            delete_tree(child);
        } else {
            SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(child);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return RemoveDirectoryA(path);
}

void handle_delete(SOCKET sock, const char *args)
{
    if (!args || !args[0]) {
        send_error_response(sock, "DELETE requires a path");
        return;
    }

    /* Try as file first, then as directory tree */
    if (DeleteFileA(args)) {
        log_msg(LOG_FILE, "DELETE: DeleteFileA(\"%s\") = OK", args);
        send_text_response(sock, "OK");
    } else if (GetFileAttributesA(args) & FILE_ATTRIBUTE_DIRECTORY) {
        if (delete_tree(args)) {
            log_msg(LOG_FILE, "DELETE: delete_tree(\"%s\") = OK", args);
            send_text_response(sock, "OK");
        } else {
            char err[256];
            DWORD gle = GetLastError();
            log_msg(LOG_FILE, "DELETE: delete_tree(\"%s\") failed, error %lu", args, (unsigned long)gle);
            _snprintf(err, sizeof(err), "delete failed: error %lu", gle);
            send_error_response(sock, err);
        }
    } else {
        char err[256];
        DWORD gle = GetLastError();
        log_msg(LOG_FILE, "DELETE: DeleteFileA(\"%s\") failed, error %lu", args, (unsigned long)gle);
        _snprintf(err, sizeof(err), "delete failed: error %lu", gle);
        send_error_response(sock, err);
    }
}
