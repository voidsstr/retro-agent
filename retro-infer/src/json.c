#include <stdlib.h>
#include <string.h>
#include "json.h"

typedef struct {
    char *p;
    int err;
} jctx_t;

static jnode_t *parse_value(jctx_t *c);

static jnode_t *node_new(jtype_t t)
{
    jnode_t *n = (jnode_t *)calloc(1, sizeof(jnode_t));
    if (n)
        n->type = t;
    return n;
}

static void skip_ws(jctx_t *c)
{
    while (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')
        c->p++;
}

/* Parses a quoted string in place: unescapes into the same buffer, NUL-
 * terminates, returns start. Only \" \\ \/ \n \t \r escapes (manifests are
 * ASCII; \uXXXX unsupported → parse error). */
static char *parse_string(jctx_t *c)
{
    char *out, *start;
    if (*c->p != '"') {
        c->err = 1;
        return NULL;
    }
    c->p++;
    start = out = c->p;
    while (*c->p && *c->p != '"') {
        if (*c->p == '\\') {
            c->p++;
            switch (*c->p) {
            case '"': *out++ = '"'; break;
            case '\\': *out++ = '\\'; break;
            case '/': *out++ = '/'; break;
            case 'n': *out++ = '\n'; break;
            case 't': *out++ = '\t'; break;
            case 'r': *out++ = '\r'; break;
            default: c->err = 1; return NULL;
            }
            c->p++;
        } else {
            *out++ = *c->p++;
        }
    }
    if (*c->p != '"') {
        c->err = 1;
        return NULL;
    }
    c->p++;          /* past closing quote */
    *out = '\0';
    return start;
}

static jnode_t *parse_container(jctx_t *c, int is_obj)
{
    jnode_t *n = node_new(is_obj ? J_OBJ : J_ARR);
    jnode_t *tail = NULL;
    char open = is_obj ? '{' : '[', close = is_obj ? '}' : ']';

    if (!n || *c->p != open) {
        c->err = 1;
        free(n);
        return NULL;
    }
    c->p++;
    skip_ws(c);
    if (*c->p == close) {
        c->p++;
        return n;
    }
    for (;;) {
        char *key = NULL;
        jnode_t *v;
        skip_ws(c);
        if (is_obj) {
            key = parse_string(c);
            if (c->err)
                break;
            skip_ws(c);
            if (*c->p != ':') {
                c->err = 1;
                break;
            }
            c->p++;
        }
        v = parse_value(c);
        if (c->err || !v)
            break;
        v->key = key;
        if (tail)
            tail->next = v;
        else
            n->child = v;
        tail = v;
        skip_ws(c);
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == close) {
            c->p++;
            return n;
        }
        c->err = 1;
        break;
    }
    json_free(n);
    return NULL;
}

static jnode_t *parse_value(jctx_t *c)
{
    jnode_t *n;
    skip_ws(c);
    switch (*c->p) {
    case '{':
        return parse_container(c, 1);
    case '[':
        return parse_container(c, 0);
    case '"': {
        char *s = parse_string(c);
        if (c->err)
            return NULL;
        n = node_new(J_STR);
        if (n)
            n->str = s;
        return n;
    }
    case 't':
        if (strncmp(c->p, "true", 4) == 0) {
            c->p += 4;
            n = node_new(J_BOOL);
            if (n)
                n->num = 1;
            return n;
        }
        c->err = 1;
        return NULL;
    case 'f':
        if (strncmp(c->p, "false", 5) == 0) {
            c->p += 5;
            return node_new(J_BOOL);
        }
        c->err = 1;
        return NULL;
    case 'n':
        if (strncmp(c->p, "null", 4) == 0) {
            c->p += 4;
            return node_new(J_NULL);
        }
        c->err = 1;
        return NULL;
    default: {
        char *end;
        double d = strtod(c->p, &end);
        if (end == c->p) {
            c->err = 1;
            return NULL;
        }
        c->p = end;
        n = node_new(J_NUM);
        if (n)
            n->num = d;
        return n;
    }
    }
}

jnode_t *json_parse(char *text)
{
    jctx_t c;
    jnode_t *root;
    c.p = text;
    c.err = 0;
    root = parse_value(&c);
    if (c.err || !root)
        return NULL;
    skip_ws(&c);
    if (*c.p != '\0') {
        json_free(root);
        return NULL;
    }
    return root;
}

void json_free(jnode_t *root)
{
    jnode_t *ch, *nx;
    if (!root)
        return;
    for (ch = root->child; ch; ch = nx) {
        nx = ch->next;
        json_free(ch);
    }
    free(root);
}

const jnode_t *json_get(const jnode_t *obj, const char *key)
{
    const jnode_t *ch;
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (ch = obj->child; ch; ch = ch->next)
        if (ch->key && strcmp(ch->key, key) == 0)
            return ch;
    return NULL;
}

double json_num(const jnode_t *obj, const char *key, double dflt)
{
    const jnode_t *n = json_get(obj, key);
    return (n && (n->type == J_NUM || n->type == J_BOOL)) ? n->num : dflt;
}

const char *json_str(const jnode_t *obj, const char *key, const char *dflt)
{
    const jnode_t *n = json_get(obj, key);
    return (n && n->type == J_STR) ? n->str : dflt;
}

int json_arr_len(const jnode_t *arr)
{
    int n = 0;
    const jnode_t *ch;
    if (!arr || arr->type != J_ARR)
        return 0;
    for (ch = arr->child; ch; ch = ch->next)
        n++;
    return n;
}

const jnode_t *json_arr_at(const jnode_t *arr, int i)
{
    const jnode_t *ch;
    if (!arr || arr->type != J_ARR)
        return NULL;
    for (ch = arr->child; ch && i > 0; ch = ch->next, i--)
        ;
    return ch;
}
