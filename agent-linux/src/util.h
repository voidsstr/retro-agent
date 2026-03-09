#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stdlib.h>

/*
 * Simple JSON builder - append-only, no parsing needed.
 * Uses a growable buffer allocated from heap (malloc/realloc).
 */

typedef struct {
    char    *buf;
    uint32_t len;
    uint32_t cap;
    int      depth;        /* nesting level for comma tracking */
    int      need_comma;   /* need comma before next element */
} json_t;

void  json_init(json_t *j);
void  json_free(json_t *j);
char *json_finish(json_t *j);  /* returns the buffer, caller must free() */

void  json_object_start(json_t *j);
void  json_object_end(json_t *j);
void  json_array_start(json_t *j);
void  json_array_end(json_t *j);

void  json_key(json_t *j, const char *key);
void  json_str(json_t *j, const char *val);
void  json_int(json_t *j, int val);
void  json_uint(json_t *j, unsigned long val);
void  json_bool(json_t *j, int val);
void  json_null(json_t *j);

/* Convenience: key + value in one call */
void  json_kv_str(json_t *j, const char *key, const char *val);
void  json_kv_int(json_t *j, const char *key, int val);
void  json_kv_uint(json_t *j, const char *key, unsigned long val);
void  json_kv_bool(json_t *j, const char *key, int val);

/* String helpers */
void  safe_strncpy(char *dst, const char *src, int maxlen);
int   str_starts_with(const char *str, const char *prefix);
const char *str_skip_spaces(const char *s);

#endif /* UTIL_H */
