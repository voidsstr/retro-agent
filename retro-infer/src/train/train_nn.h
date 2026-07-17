#ifndef RETRO_INFER_TRAIN_NN_H
#define RETRO_INFER_TRAIN_NN_H

typedef struct {
    const char *train_x, *train_y, *test_x, *test_y;
    int n_train, n_test;
    const char *arch;          /* "784,128,10" */
    int epochs, batch, seed;
    float lr, momentum;
    const char *out_rim;       /* "-" = don't save */
    int input_u8_div255;       /* saved model: input u8 with div 255 */
} train_nn_cfg_t;

int train_nn_run(const train_nn_cfg_t *cfg);

/* rim_save.c: write a dense-f32 .rim (dense+relu...+softmax) */
int rim_save_dense(const char *path, const int *dims, int n_layers,
                   const float *const *W, const float *const *b,
                   int input_u8_div255);

#endif
