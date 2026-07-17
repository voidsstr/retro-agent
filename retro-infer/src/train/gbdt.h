#ifndef RETRO_INFER_GBDT_H
#define RETRO_INFER_GBDT_H

typedef struct {
    const char *features, *labels;   /* u8 [N,F]; u8 or f32 (regress) */
    int n, f;
    float val_frac;                  /* last fraction = validation */
    int rounds, depth, min_child;
    float lr, lambda;
    int regress;
} gbdt_cfg_t;

int gbdt_run(const gbdt_cfg_t *cfg);

typedef struct {
    const char *features, *labels;
    int n, f;
    float val_frac;
    int n_trees, depth, min_child, seed;
} forest_cfg_t;

int forest_run(const forest_cfg_t *cfg);

typedef struct {
    const char *features, *labels;
    int n, f;
    float val_frac;
    int epochs, seed;
    float lr, reg;                   /* Pegasos-style lambda */
} svm_cfg_t;

int svm_run(const svm_cfg_t *cfg);

#endif
