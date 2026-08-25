/* test_json_escape.c — TRUE-SOURCE test: compiles the REAL JSON escaper from
 * agent/src/util.c against the fake Win32 in stubs/ and checks what it emits.
 *
 * Encodes the 2026-08-25 fix. Every agent command that returns JSON
 * (SYSINFO, SMARTINFO, DIRLIST, PROCLIST, GAMEINDEX, ...) goes through this
 * one function, and it used to write bytes >= 0x80 straight through. A path
 * from the box's ANSI codepage therefore produced a document that is not
 * valid UTF-8, and json.loads() on the host raised instead of parsing.
 *
 * This was not hypothetical: GAMEINDEX on .240 returned the directory
 * "C:\Games\Battlefield.1942\Battlefield.1942.PC.Game(djDEVASTATE\x92)".
 * The high byte is a cp1252 curly quote and it would have broken the game
 * index for that whole machine.
 *
 * The fix escapes >= 0x80 as \u00XX, which keeps the exact byte value (a
 * consumer wanting the original bytes re-encodes latin-1) and keeps the
 * document pure ASCII. Assert BOTH the fixed behaviour and that the
 * old-buggy raw passthrough is gone.
 */
#include "munit.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Claim the shared stub's include guard so <windows.h> resolves to nothing,
 * then supply exactly the Win32 surface util.c touches. The shared
 * stubs/windows.h is deliberately empty (crypto.c needs no Win32 at all) and
 * util.c needs the heap, so shim it HERE rather than growing the shared stub
 * and risking the other true-source tests. */
#define STUB_WINDOWS_H
typedef unsigned long DWORD;
typedef int           BOOL;
typedef void         *HANDLE;
typedef void         *HMODULE;
#define WINAPI
#define _snprintf snprintf

/* A malloc-backed process heap is enough: the escaper only grows a buffer. */
static HANDLE GetProcessHeap(void) { return (HANDLE)1; }
static void  *HeapAlloc(HANDLE h, DWORD f, size_t n)
{ (void)h; (void)f; return malloc(n); }
static void  *HeapReAlloc(HANDLE h, DWORD f, void *p, size_t n)
{ (void)h; (void)f; return realloc(p, n); }
static BOOL   HeapFree(HANDLE h, DWORD f, void *p)
{ (void)h; (void)f; free(p); return 1; }
/* set_handle_noinherit() resolves SetHandleInformation dynamically; returning
 * NULL from these makes it the no-op it already is on Win9x. */
static HMODULE GetModuleHandleA(const char *n) { (void)n; return NULL; }
static void   *GetProcAddress(HMODULE m, const char *n)
{ (void)m; (void)n; return NULL; }

#include "../../agent/src/util.c"

/* Escape one string through the real builder and return the finished doc. */
static char *emit(const char *raw, char *out, size_t outlen)
{
    json_t j;
    char  *doc;
    json_init(&j);
    json_object_start(&j);
    json_kv_str(&j, "v", raw);
    json_object_end(&j);
    doc = json_finish(&j);
    strncpy(out, doc ? doc : "", outlen - 1);
    out[outlen - 1] = 0;
    if (doc)
        HeapFree(GetProcessHeap(), 0, doc);
    return out;
}

TEST(high_bytes_are_escaped_not_passed_through) {
    char got[256];
    /* The real .240 directory name, trimmed to the part that matters. */
    emit("Game(djDEVASTATE\x92)", got, sizeof(got));
    CHECK(strstr(got, "\\u0092") != NULL,
          "byte 0x92 must be escaped as \\u0092");
    CHECK(strchr(got, (char)0x92) == NULL,
          "the raw high byte must NOT survive - that is the bug: it makes the "
          "document invalid UTF-8 and json.loads() raises");
}

TEST(every_high_byte_round_trips_as_its_own_code_point) {
    int i;
    for (i = 0x80; i <= 0xFF; i++) {
        char raw[2], got[256], want[16];
        raw[0] = (char)i;
        raw[1] = 0;
        emit(raw, got, sizeof(got));
        sprintf(want, "\\u%04x", i);
        CHECK(strstr(got, want) != NULL,
              "each high byte escapes to its own \\u00XX, losing nothing");
    }
}

TEST(control_bytes_still_escaped) {
    char got[256];
    emit("a\x01" "b", got, sizeof(got));
    CHECK(strstr(got, "\\u0001") != NULL, "control bytes were already escaped");
}

TEST(plain_ascii_is_untouched) {
    char got[256];
    emit("C:\\Games\\Quake2", got, sizeof(got));
    /* Backslashes double, nothing else changes, and no \u escapes appear. */
    CHECK(strstr(got, "C:\\\\Games\\\\Quake2") != NULL,
          "ordinary paths still escape only the backslash");
    CHECK(strstr(got, "\\u") == NULL,
          "pure-ASCII input must not gain any \\u escapes");
}

TEST(json_control_characters_keep_their_short_forms) {
    char got[256];
    emit("a\"b\nc\td", got, sizeof(got));
    CHECK(strstr(got, "\\\"") != NULL, "quote");
    CHECK(strstr(got, "\\n") != NULL, "newline keeps the short form");
    CHECK(strstr(got, "\\t") != NULL, "tab keeps the short form");
}

MUNIT_MAIN("json escaper high-byte escaping (fix 2026-08-25)", {
    RUN(high_bytes_are_escaped_not_passed_through);
    RUN(every_high_byte_round_trips_as_its_own_code_point);
    RUN(control_bytes_still_escaped);
    RUN(plain_ascii_is_untouched);
    RUN(json_control_characters_keep_their_short_forms);
})
