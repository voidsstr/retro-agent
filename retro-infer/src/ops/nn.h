#ifndef RETRO_INFER_NN_H
#define RETRO_INFER_NN_H

#include <stddef.h>

float ri_round(float x);
signed char ri_quant_clamp(float x);

void im2col_f32(const float *in, int C, int H, int W, int k, int stride,
                int pad, float *out, int oH, int oW);
void im2col_i8(const signed char *in, int C, int H, int W, int k, int stride,
               int pad, signed char *out, int oH, int oW);

void relu_f32(float *x, size_t n);
void relu_i8(signed char *x, size_t n);
void sigmoid_f32(float *x, size_t n);
void tanh_f32(float *x, size_t n);
void softmax_f32(float *x, size_t n);

void maxpool_f32(const float *in, int C, int H, int W, int k, int s,
                 float *out, int oH, int oW);
void maxpool_i8(const signed char *in, int C, int H, int W, int k, int s,
                signed char *out, int oH, int oW);

void bias_requant_i8(const int *acc, int out_ch, int n, const int *bias,
                     float mult, signed char *out);
void bias_dequant_f32(const int *acc, int out_ch, int n, const int *bias,
                      float scale, float *out);
void bias_add_f32(float *x, int out_ch, int n, const float *bias);

#endif
