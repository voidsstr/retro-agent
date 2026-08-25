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
            /* Escape BOTH control bytes and anything >= 0x80. A raw high byte
             * comes straight from the box's ANSI codepage, so emitting it
             * verbatim produces a document that is not valid UTF-8 and blows
             * up json.loads on the host. Real case: a fleet box with a game
             * directory named "Battlefield.1942.PC.Game(djDEVASTATE\x92)".
             * \u00XX keeps the exact byte value (a consumer that wants the
             * original bytes can re-encode latin-1) and keeps the document
             * pure ASCII, which every existing consumer already handles. */
            if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x80) {
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

/* SetHandleInformation requires Windows 2000+ (confirmed via MSDN's own
 * "Minimum supported client" — it does not exist on Windows 95/98/ME at
 * all). Calling it as a plain static import meant every genuine Win9x
 * box could never even LOAD retro_agent.exe: the loader can't resolve
 * the symbol at process-creation time, before main() or any logging
 * runs — this is what actually caused the totally silent, zero-log
 * startup failure on a real Windows 98 box (a Compaq Deskpro 2000),
 * not (only) the is_nt()-guard gap fixed earlier in service.c. Resolve
 * it dynamically instead, matching the same LoadLibrary+GetProcAddress
 * pattern already used elsewhere in this codebase (netshare.c's
 * mpr.dll, retrowall.c's uxtheme.dll, service.c's advapi32.dll) — a
 * no-op on Win9x rather than an unresolvable import. kernel32.dll is
 * always already loaded in every Win32 process, so GetModuleHandleA
 * (not LoadLibraryA) is enough to get a handle to look the symbol up in. */
void set_handle_noinherit(HANDLE h)
{
    typedef BOOL (WINAPI *pfn_SetHandleInformation)(HANDLE, DWORD, DWORD);
    static pfn_SetHandleInformation p = NULL;
    static int resolved = 0;

    if (!resolved) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32)
            p = (pfn_SetHandleInformation)
                GetProcAddress(k32, "SetHandleInformation");
        resolved = 1;
    }
    if (p)
        p(h, 1 /* HANDLE_FLAG_INHERIT */, 0);
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
