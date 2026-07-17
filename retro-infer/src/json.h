/* Minimal JSON DOM parser for .rim manifests. No CRT surprises, ~free of
 * allocations beyond one node pool. Numbers are doubles; strings point into
 * a NUL-patched copy of the input (parser owns nothing else). */
#ifndef RETRO_JSON_H
#define RETRO_JSON_H

typedef enum {
    J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ
} jtype_t;

typedef struct jnode {
    jtype_t type;
    const char *key;        /* set when parent is an object */
    double num;             /* J_NUM / J_BOOL */
    const char *str;        /* J_STR */
    struct jnode *child;    /* first child (J_ARR/J_OBJ) */
    struct jnode *next;     /* next sibling */
} jnode_t;

/* Parses text (MUTATED in place for string NUL-termination; keep it alive
 * as long as the tree). Returns root or NULL on parse error. */
jnode_t *json_parse(char *text);
void json_free(jnode_t *root);

const jnode_t *json_get(const jnode_t *obj, const char *key);
double json_num(const jnode_t *obj, const char *key, double dflt);
const char *json_str(const jnode_t *obj, const char *key, const char *dflt);
int json_arr_len(const jnode_t *arr);
const jnode_t *json_arr_at(const jnode_t *arr, int i);

#endif
