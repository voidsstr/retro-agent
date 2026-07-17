/*
 * Layer ops used by the executor: im2col conv2d (f32 + int8), dense, pooling,
 * activations, softmax. Batch = 1 everywhere (inference); training adds its
 * own paths in M2.
 *
 * Rounding: RI_ROUND is half-away-from-zero to match the Python reference
 * (tools/rim/FORMAT.md). Do not switch to rint()/banker's rounding.
 */
#include <math.h>
#include <string.h>
#include "../infer.h"
#include "nn.h"

float ri_round(float x)
{
    return (x >= 0.0f) ? (float)floor((double)x + 0.5)
                       : -(float)floor(-(double)x + 0.5);
}

signed char ri_quant_clamp(float x)
{
    float r = ri_round(x);
    if (r > 127.0f)
        return 127;
    if (r < -127.0f)
        return -127;
    return (signed char)r;
}

/* ---- im2col: (C,H,W) -> [C*k*k, oH*oW], zero padding ---- */

void im2col_f32(const float *in, int C, int H, int W, int k, int stride,
                int pad, float *out, int oH, int oW)
{
    int c, ky, kx, oy, ox;
    int row = 0;
    for (c = 0; c < C; c++) {
        for (ky = 0; ky < k; ky++) {
            for (kx = 0; kx < k; kx++, row++) {
                float *dst = out + (size_t)row * oH * oW;
                for (oy = 0; oy < oH; oy++) {
                    int iy = oy * stride - pad + ky;
                    for (ox = 0; ox < oW; ox++) {
                        int ix = ox * stride - pad + kx;
                        float v = 0.0f;
                        if (iy >= 0 && iy < H && ix >= 0 && ix < W)
                            v = in[((size_t)c * H + iy) * W + ix];
                        dst[oy * oW + ox] = v;
                    }
                }
            }
        }
    }
}

void im2col_i8(const signed char *in, int C, int H, int W, int k, int stride,
               int pad, signed char *out, int oH, int oW)
{
    int c, ky, kx, oy, ox;
    int row = 0;
    for (c = 0; c < C; c++) {
        for (ky = 0; ky < k; ky++) {
            for (kx = 0; kx < k; kx++, row++) {
                signed char *dst = out + (size_t)row * oH * oW;
                for (oy = 0; oy < oH; oy++) {
                    int iy = oy * stride - pad + ky;
                    for (ox = 0; ox < oW; ox++) {
                        int ix = ox * stride - pad + kx;
                        signed char v = 0;
                        if (iy >= 0 && iy < H && ix >= 0 && ix < W)
                            v = in[((size_t)c * H + iy) * W + ix];
                        dst[oy * oW + ox] = v;
                    }
                }
            }
        }
    }
}

/* ---- elementwise ---- */

void relu_f32(float *x, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (x[i] < 0.0f)
            x[i] = 0.0f;
}

void relu_i8(signed char *x, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (x[i] < 0)
            x[i] = 0;
}

void sigmoid_f32(float *x, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        x[i] = (float)(1.0 / (1.0 + exp(-(double)x[i])));
}

void tanh_f32(float *x, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        x[i] = (float)tanh((double)x[i]);
}

void softmax_f32(float *x, size_t n)
{
    size_t i;
    double sum = 0.0;
    float mx = x[0];
    for (i = 1; i < n; i++)
        if (x[i] > mx)
            mx = x[i];
    for (i = 0; i < n; i++) {
        double e = exp((double)(x[i] - mx));
        x[i] = (float)e;
        sum += e;
    }
    for (i = 0; i < n; i++)
        x[i] = (float)((double)x[i] / sum);
}

/* ---- pooling: (C,H,W) -> (C,oH,oW), kernel k stride s, no padding ---- */

void maxpool_f32(const float *in, int C, int H, int W, int k, int s,
                 float *out, int oH, int oW)
{
    int c, oy, ox, ky, kx;
    for (c = 0; c < C; c++) {
        for (oy = 0; oy < oH; oy++) {
            for (ox = 0; ox < oW; ox++) {
                float m = -3.4e38f;
                for (ky = 0; ky < k; ky++) {
                    int iy = oy * s + ky;
                    if (iy >= H) continue;
                    for (kx = 0; kx < k; kx++) {
                        int ix = ox * s + kx;
                        float v;
                        if (ix >= W) continue;
                        v = in[((size_t)c * H + iy) * W + ix];
                        if (v > m)
                            m = v;
                    }
                }
                out[((size_t)c * oH + oy) * oW + ox] = m;
            }
        }
    }
}

void maxpool_i8(const signed char *in, int C, int H, int W, int k, int s,
                signed char *out, int oH, int oW)
{
    int c, oy, ox, ky, kx;
    for (c = 0; c < C; c++) {
        for (oy = 0; oy < oH; oy++) {
            for (ox = 0; ox < oW; ox++) {
                signed char m = -128;
                for (ky = 0; ky < k; ky++) {
                    int iy = oy * s + ky;
                    if (iy >= H) continue;
                    for (kx = 0; kx < k; kx++) {
                        int ix = ox * s + kx;
                        signed char v;
                        if (ix >= W) continue;
                        v = in[((size_t)c * H + iy) * W + ix];
                        if (v > m)
                            m = v;
                    }
                }
                out[((size_t)c * oH + oy) * oW + ox] = m;
            }
        }
    }
}

/* ---- bias + requant helpers for int8 layers ----
 * acc[out_ch][n] i32; bias i32 per out_ch.
 * If out_scale > 0: requant to i8 with multiplier m = in_scale*w_scale/out_scale.
 * If out_scale == 0: dequantize to f32 with in_scale*w_scale. */

void bias_requant_i8(const int *acc, int out_ch, int n, const int *bias,
                     float mult, signed char *out)
{
    int c, j;
    for (c = 0; c < out_ch; c++) {
        for (j = 0; j < n; j++) {
            float v = (float)(acc[(size_t)c * n + j] + (bias ? bias[c] : 0));
            out[(size_t)c * n + j] = ri_quant_clamp(v * mult);
        }
    }
}

void bias_dequant_f32(const int *acc, int out_ch, int n, const int *bias,
                      float scale, float *out)
{
    int c, j;
    for (c = 0; c < out_ch; c++) {
        for (j = 0; j < n; j++)
            out[(size_t)c * n + j] =
                (float)(acc[(size_t)c * n + j] + (bias ? bias[c] : 0)) * scale;
    }
}

void bias_add_f32(float *x, int out_ch, int n, const float *bias)
{
    int c, j;
    if (!bias)
        return;
    for (c = 0; c < out_ch; c++)
        for (j = 0; j < n; j++)
            x[(size_t)c * n + j] += bias[c];
}
