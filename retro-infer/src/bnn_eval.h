#ifndef RETRO_INFER_BNN_EVAL_H
#define RETRO_INFER_BNN_EVAL_H

/* --bnn-eval driver (M5): backend = "cpu" or "glide" */
int bnn_eval(const char *mpath, const char *ipath, const char *lpath, int N,
             const char *backend);

#endif
