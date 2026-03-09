#include "util.h"
#include <string.h>
#include <stdio.h>

#define JSON_INITIAL_CAP  4096

static void json_ensure(json_t *j, DWORD extra)
{
    DWORD needed = j->len + extra;
    if (needed <= j->cap) return;

    while (j->cap < needed)
        j->cap *= 2;

    j->buf = (char *)HeapReAlloc(GetProcessHeap(), 0, j->buf, j->cap);
}

static void json_append(json_t *j, const char *s, DWORD slen)
{
    json_ensure(j, slen);
    memcpy(j->buf + j->len, s, slen);
    j->len += slen;
}

static void json_append_str(json_t *j, const char *s)
{
    json_append(j, s, (DWORD)strlen(s));
}

static void json_comma_if_needed(json_t *j)
{
    if (j->need_comma)
        json_append(j, ",", 1);
    j->need_comma = 0;
}

void json_init(json_t *j)
{
    j->cap = JSON_INITIAL_CAP;
    j->buf = (char *)HeapAlloc(GetProcessHeap(), 0, j->cap);
    j->len = 0;
    j->depth = 0;
    j->need_comma = 0;
}

void json_free(json_t *j)
{
    if (j->buf) {
        HeapFree(GetProcessHeap(), 0, j->buf);
        j->buf = NULL;
    }
}

char *json_finish(json_t *j)
{
    json_ensure(j, 1);
    j->buf[j->len] = '\0';
    return j->buf;
}

void json_object_start(json_t *j)
{
    json_comma_if_needed(j);
    json_append(j, "{", 1);
    j->depth++;
    j->need_comma = 0;
}

void json_object_end(json_t *j)
{
    json_append(j, "}", 1);
    j->depth--;
    j->need_comma = 1;
}

void json_array_start(json_t *j)
{
    json_comma_if_needed(j);
    json_append(j, "[", 1);
    j->depth++;
    j->need_comma = 0;
}

void json_array_end(json_t *j)
{
    json_append(j, "]", 1);
    j->depth--;
    j->need_comma = 1;
}

/* Write a JSON-safe escaped string */
static void json_write_escaped(json_t *j, const char *s)
{
    json_append(j, "\"", 1);
    while (*s) {
        switch (*s) {
        case '"':  json_append(j, "\\\"", 2); break;
        case '\\': json_append(j, "\\\\", 2); break;
        case '\b': json_append(j, "\\b", 2); break;
        case '\f': json_append(j, "\\f", 2); break;
        case '\n': json_append(j, "\\n", 2); break;
        case '\r': json_append(j, "\\r", 2); break;
        case '\t': json_append(j, "\\t", 2); break;
        default:
            if ((unsigned char)*s < 0x20) {
                char esc[8];
                _snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*s);
                json_append_str(j, esc);
            } else {
                json_append(j, s, 1);
            }
        }
        s++;
    }
    json_append(j, "\"", 1);
}

void json_key(json_t *j, const char *key)
{
    json_comma_if_needed(j);
    json_write_escaped(j, key);
    json_append(j, ":", 1);
    j->need_comma = 0;  /* value follows, no comma yet */
}

void json_str(json_t *j, const char *val)
{
    json_comma_if_needed(j);
    if (val)
        json_write_escaped(j, val);
    else
        json_append_str(j, "null");
    j->need_comma = 1;
}

void json_int(json_t *j, int val)
{
    char num[32];
    json_comma_if_needed(j);
    _snprintf(num, sizeof(num), "%d", val);
    json_append_str(j, num);
    j->need_comma = 1;
}

void json_uint(json_t *j, DWORD val)
{
    char num[32];
    json_comma_if_needed(j);
    _snprintf(num, sizeof(num), "%lu", (unsigned long)val);
    json_append_str(j, num);
    j->need_comma = 1;
}

void json_bool(json_t *j, int val)
{
    json_comma_if_needed(j);
    json_append_str(j, val ? "true" : "false");
    j->need_comma = 1;
}

void json_null(json_t *j)
{
    json_comma_if_needed(j);
    json_append_str(j, "null");
    j->need_comma = 1;
}

void json_kv_str(json_t *j, const char *key, const char *val)
{
    json_key(j, key);
    json_str(j, val);
}

void json_kv_int(json_t *j, const char *key, int val)
{
    json_key(j, key);
    json_int(j, val);
}

void json_kv_uint(json_t *j, const char *key, DWORD val)
{
    json_key(j, key);
    json_uint(j, val);
}

void json_kv_bool(json_t *j, const char *key, int val)
{
    json_key(j, key);
    json_bool(j, val);
}

/* String helpers */

void safe_strncpy(char *dst, const char *src, int maxlen)
{
    if (maxlen <= 0) return;
    strncpy(dst, src, maxlen - 1);
    dst[maxlen - 1] = '\0';
}

int str_starts_with(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

const char *str_skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

void format_hex16(char *buf, DWORD val)
{
    _snprintf(buf, 8, "0x%04lX", (unsigned long)(val & 0xFFFF));
}

void format_hex32(char *buf, DWORD val)
{
    _snprintf(buf, 12, "0x%08lX", (unsigned long)val);
}
