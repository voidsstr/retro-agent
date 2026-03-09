#ifndef UTIL_H
#define UTIL_H

#include <windows.h>

/*
 * Simple JSON builder - append-only, no parsing needed.
 * Uses a growable buffer allocated from process heap.
 */

typedef struct {
    char  *buf;
    DWORD  len;
    DWORD  cap;
    int    depth;        /* nesting level for comma tracking */
    int    need_comma;   /* need comma before next element */
} json_t;

void  json_init(json_t *j);
void  json_free(json_t *j);
char *json_finish(json_t *j);  /* returns the buffer, caller must HeapFree */

void  json_object_start(json_t *j);
void  json_object_end(json_t *j);
void  json_array_start(json_t *j);
void  json_array_end(json_t *j);

void  json_key(json_t *j, const char *key);
void  json_str(json_t *j, const char *val);
void  json_int(json_t *j, int val);
void  json_uint(json_t *j, DWORD val);
void  json_bool(json_t *j, int val);
void  json_null(json_t *j);

/* Convenience: key + value in one call */
void  json_kv_str(json_t *j, const char *key, const char *val);
void  json_kv_int(json_t *j, const char *key, int val);
void  json_kv_uint(json_t *j, const char *key, DWORD val);
void  json_kv_bool(json_t *j, const char *key, int val);

/* String helpers */
void  safe_strncpy(char *dst, const char *src, int maxlen);
int   str_starts_with(const char *str, const char *prefix);
const char *str_skip_spaces(const char *s);

/* Hex formatting */
void  format_hex16(char *buf, DWORD val);  /* "0x1234" */
void  format_hex32(char *buf, DWORD val);  /* "0x12345678" */

#endif /* UTIL_H */
