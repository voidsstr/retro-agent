#ifndef RETRO_INFER_EXEC_H
#define RETRO_INFER_EXEC_H

#include <stddef.h>

typedef struct model model_t;

int model_open(const char *path, model_t **out, char *errbuf, size_t errlen);
void model_close(model_t *m);

int model_n_classes(const model_t *m);
/* stop before a trailing softmax layer (pre-softmax logit dumps) */
void model_set_skip_softmax(model_t *m, int skip);
int model_input_bytes(const model_t *m);   /* bytes per sample on disk */
const char *model_name(const model_t *m);

/* input: model_input_bytes() raw bytes; logits: model_n_classes() floats */
int model_infer(model_t *m, const void *input, float *logits);

#endif
