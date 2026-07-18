/*
 * retro-infer — dependency-free ML engine for the retro fleet.
 *
 *   retro-infer --selfcheck
 *   retro-infer --version
 *   retro-infer --riminfo <model.rim>
 *   retro-infer --infer <model.rim> <input.bin>
 *   retro-infer --eval <model.rim> <images.bin> <labels.bin> <N>
 *               [--logits <out.bin>] [--scalar]
 *
 * --eval prints top-1 accuracy + throughput; --logits dumps N*n_classes fp32
 * for parity comparison against tools/rim/eval_ref.py. --scalar forces the
 * scalar kernels (A/B and parity vs vectorized paths).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "infer.h"
#include "exec.h"
#include "port.h"
#include "train/train_nn.h"
#include "train/gbdt.h"
#include "serve.h"
#include "gpu/glide_mac.h"
#include "gpu/nv_gl.h"
#include "bnn_eval.h"

static void print_selfcheck(void)
{
    cpu_caps_t caps;
    double gflops_active, gflops_scalar;
    kernels_t saved;

    cpu_detect(&caps);
    kernels_init(&caps);

    printf("retro-infer v%s selfcheck\n", INFER_VERSION);
    printf("cpu.vendor=%s\n", caps.has_cpuid ? caps.vendor : "(no cpuid)");
    printf("cpu.brand=%s\n", caps.brand[0] ? caps.brand : "(none)");
    printf("cpu.family=%u model=%u stepping=%u\n",
           caps.family, caps.model, caps.stepping);
    printf("isa.mmx=%d isa.sse=%d isa.sse2=%d isa.3dnow=%d isa.3dnowext=%d\n",
           caps.mmx, caps.sse, caps.sse2, caps.amd3dnow, caps.amd3dnowext);

    printf("ram.total_mb=%lu ram.avail_mb=%lu\n", ri_ram_mb(0), ri_ram_mb(1));

    printf("kernel.gemm_f32=%s kernel.gemm_i8=%s\n",
           g_kernels.gemm_f32_name, g_kernels.gemm_i8_name);

    printf("bench: gemm 256x256x256 fp32 ...\n");
    fflush(stdout);
    gflops_active = bench_gemm_gflops(256, 4);
    printf("bench.gflops.%s=%.3f\n", g_kernels.gemm_f32_name, gflops_active);

    if (strcmp(g_kernels.gemm_f32_name, "scalar") != 0) {
        saved = g_kernels;
        kernels_force_scalar();
        gflops_scalar = bench_gemm_gflops(256, 4);
        g_kernels = saved;
        printf("bench.gflops.scalar=%.3f\n", gflops_scalar);
    }
    printf("selfcheck: OK\n");
}

static int print_riminfo(const char *path)
{
    rim_model_t m;
    char err[128];
    if (rim_load(path, &m, err, sizeof(err)) != 0) {
        printf("riminfo: FAIL: %s\n", err);
        return 1;
    }
    printf("riminfo: OK manifest_len=%lu weights_len=%lu\n",
           (unsigned long)m.manifest_len, (unsigned long)m.weights_len);
    printf("%s\n", m.manifest_json);
    rim_free(&m);
    return 0;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    unsigned char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static int do_infer(const char *mpath, const char *ipath)
{
    model_t *m;
    char err[128];
    size_t ilen;
    unsigned char *input;
    float *logits;
    int i, nc, best;

    if (model_open(mpath, &m, err, sizeof(err)) != 0) {
        printf("infer: FAIL: %s\n", err);
        return 1;
    }
    input = read_file(ipath, &ilen);
    if (!input || (int)ilen < model_input_bytes(m)) {
        printf("infer: FAIL: input file too small (need %d bytes)\n",
               model_input_bytes(m));
        model_close(m);
        free(input);
        return 1;
    }
    nc = model_n_classes(m);
    logits = (float *)malloc((size_t)nc * sizeof(float));
    if (!logits || model_infer(m, input, logits) != 0) {
        printf("infer: FAIL: inference error\n");
        model_close(m);
        free(input);
        free(logits);
        return 1;
    }
    best = 0;
    for (i = 1; i < nc; i++)
        if (logits[i] > logits[best])
            best = i;
    printf("model=%s class=%d\n", model_name(m), best);
    for (i = 0; i < nc; i++)
        printf("logit[%d]=%.6f\n", i, (double)logits[i]);
    model_close(m);
    free(input);
    free(logits);
    return 0;
}

static int do_eval(const char *mpath, const char *ipath, const char *lpath,
                   int N, const char *logits_out)
{
    model_t *m;
    char err[128];
    size_t ilen, llen;
    unsigned char *images, *labels;
    float *logits;
    FILE *lf = NULL;
    int n, i, nc, correct = 0, per_sample;
    double t0, t1, secs;

    if (model_open(mpath, &m, err, sizeof(err)) != 0) {
        printf("eval: FAIL: %s\n", err);
        return 1;
    }
    /* logit dumps + argmax don't need the trailing softmax */
    model_set_skip_softmax(m, 1);
    per_sample = model_input_bytes(m);
    images = read_file(ipath, &ilen);
    labels = read_file(lpath, &llen);
    if (!images || !labels || (size_t)N * per_sample > ilen ||
        (size_t)N > llen) {
        printf("eval: FAIL: data files too small for N=%d\n", N);
        model_close(m);
        free(images);
        free(labels);
        return 1;
    }
    nc = model_n_classes(m);
    logits = (float *)malloc((size_t)nc * sizeof(float));
    if (!logits) {
        model_close(m);
        free(images);
        free(labels);
        return 1;
    }
    if (logits_out) {
        lf = fopen(logits_out, "wb");
        if (!lf) {
            printf("eval: FAIL: cannot open logits output\n");
            model_close(m);
            free(images);
            free(labels);
            free(logits);
            return 1;
        }
    }

    t0 = ri_now();
    for (n = 0; n < N; n++) {
        int best = 0;
        if (model_infer(m, images + (size_t)n * per_sample, logits) != 0) {
            printf("eval: FAIL: inference error at sample %d\n", n);
            if (lf) fclose(lf);
            model_close(m);
            free(images);
            free(labels);
            free(logits);
            return 1;
        }
        for (i = 1; i < nc; i++)
            if (logits[i] > logits[best])
                best = i;
        if (best == (int)labels[n])
            correct++;
        if (lf)
            fwrite(logits, sizeof(float), (size_t)nc, lf);
    }
    t1 = ri_now();
    secs = t1 - t0;
    if (lf)
        fclose(lf);

    printf("eval.model=%s\n", model_name(m));
    printf("eval.kernel_f32=%s eval.kernel_i8=%s\n",
           g_kernels.gemm_f32_name, g_kernels.gemm_i8_name);
    printf("eval.n=%d eval.correct=%d eval.top1=%.4f\n", N, correct,
           (double)correct / (double)N);
    printf("eval.secs=%.3f eval.img_per_sec=%.2f\n", secs,
           secs > 0 ? (double)N / secs : 0.0);
    printf("eval: OK\n");
    model_close(m);
    free(images);
    free(labels);
    free(logits);
    return 0;
}

int main(int argc, char **argv)
{
    cpu_caps_t caps;
    int i, force_scalar = 0, regress = 0;
    const char *logits_out = NULL;

    cpu_detect(&caps);
    kernels_init(&caps);

    /* global flags (strip from argv) */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scalar") == 0) {
            force_scalar = 1;
            memmove(&argv[i], &argv[i + 1], (size_t)(argc - i - 1) * sizeof(char *));
            argc--; i--;
        } else if (strcmp(argv[i], "--regress") == 0) {
            regress = 1;
            memmove(&argv[i], &argv[i + 1], (size_t)(argc - i - 1) * sizeof(char *));
            argc--; i--;
        } else if (strcmp(argv[i], "--logits") == 0 && i + 1 < argc) {
            logits_out = argv[i + 1];
            memmove(&argv[i], &argv[i + 2], (size_t)(argc - i - 2) * sizeof(char *));
            argc -= 2; i--;
        }
    }
    if (force_scalar)
        kernels_force_scalar();

    if (argc >= 2 && strcmp(argv[1], "--selfcheck") == 0) {
        print_selfcheck();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("retro-infer v%s\n", INFER_VERSION);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--riminfo") == 0)
        return print_riminfo(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "--infer") == 0)
        return do_infer(argv[2], argv[3]);
    if (argc >= 6 && strcmp(argv[1], "--eval") == 0)
        return do_eval(argv[2], argv[3], argv[4], atoi(argv[5]), logits_out);
    /* default 9896: the agent itself owns 9897 (AGENT_TCP_PORT_ALT) */
    if (argc >= 2 && strcmp(argv[1], "--serve") == 0)
        return serve_run(argc >= 3 ? atoi(argv[2]) : 9896);
    if (argc >= 6 && strcmp(argv[1], "--bnn-eval") == 0)
        return bnn_eval(argv[2], argv[3], argv[4], atoi(argv[5]),
                        argc >= 7 ? argv[6] : "cpu");
    if (argc >= 2 && strcmp(argv[1], "--glide-check") == 0)
        return glide_check(argc >= 3 ? atoi(argv[2]) : 64,
                           argc >= 4 ? atoi(argv[3]) : 64,
                           argc >= 5 ? atoi(argv[4]) : 128,
                           argc >= 6 ? (unsigned)atoi(argv[5]) : 42);
    if (argc >= 2 && strcmp(argv[1], "--nv-check") == 0)
        return nv_check(argc >= 3 ? atoi(argv[2]) : 64,
                        argc >= 4 ? atoi(argv[3]) : 64,
                        argc >= 5 ? atoi(argv[4]) : 128,
                        argc >= 6 ? (unsigned)atoi(argv[5]) : 42);
    if (argc >= 2 && strcmp(argv[1], "--nv-check-multi") == 0)
        return nv_check_multi(argc >= 3 ? (unsigned)atoi(argv[2]) : 42);
    if (argc >= 15 && strcmp(argv[1], "--train-mlp") == 0) {
        train_nn_cfg_t c;
        memset(&c, 0, sizeof(c));
        c.train_x = argv[2]; c.train_y = argv[3]; c.n_train = atoi(argv[4]);
        c.test_x = argv[5]; c.test_y = argv[6]; c.n_test = atoi(argv[7]);
        c.arch = argv[8]; c.epochs = atoi(argv[9]);
        c.lr = (float)atof(argv[10]); c.momentum = (float)atof(argv[11]);
        c.batch = atoi(argv[12]); c.seed = atoi(argv[13]);
        c.out_rim = argv[14];
        c.input_u8_div255 = 1;
        if (c.batch < 1 || c.batch > 1024) {
            printf("train: FAIL: batch must be 1..1024\n");
            return 1;
        }
        return train_nn_run(&c);
    }
    if (argc >= 12 && strcmp(argv[1], "--train-gbdt") == 0) {
        gbdt_cfg_t c;
        memset(&c, 0, sizeof(c));
        c.features = argv[2]; c.labels = argv[3];
        c.n = atoi(argv[4]); c.f = atoi(argv[5]);
        c.val_frac = (float)atof(argv[6]);
        c.rounds = atoi(argv[7]); c.depth = atoi(argv[8]);
        c.min_child = atoi(argv[9]);
        c.lr = (float)atof(argv[10]); c.lambda = (float)atof(argv[11]);
        c.regress = regress;
        return gbdt_run(&c);
    }
    if (argc >= 10 && strcmp(argv[1], "--train-rf") == 0) {
        forest_cfg_t c;
        memset(&c, 0, sizeof(c));
        c.features = argv[2]; c.labels = argv[3];
        c.n = atoi(argv[4]); c.f = atoi(argv[5]);
        c.val_frac = (float)atof(argv[6]);
        c.n_trees = atoi(argv[7]); c.depth = atoi(argv[8]);
        c.min_child = 1;
        c.seed = atoi(argv[9]);
        return forest_run(&c);
    }
    if (argc >= 10 && strcmp(argv[1], "--train-svm") == 0) {
        svm_cfg_t c;
        memset(&c, 0, sizeof(c));
        c.features = argv[2]; c.labels = argv[3];
        c.n = atoi(argv[4]); c.f = atoi(argv[5]);
        c.val_frac = (float)atof(argv[6]);
        c.epochs = atoi(argv[7]);
        c.lr = (float)atof(argv[8]); c.reg = (float)atof(argv[9]);
        c.seed = argc >= 11 ? atoi(argv[10]) : 42;
        return svm_run(&c);
    }

    printf("retro-infer v%s\n", INFER_VERSION);
    printf("usage: retro-infer --selfcheck | --version | --riminfo <m.rim>\n");
    printf("       retro-infer --infer <m.rim> <input.bin>\n");
    printf("       retro-infer --eval <m.rim> <imgs.bin> <lbls.bin> <N> "
           "[--logits <out.bin>] [--scalar]\n");
    printf("       retro-infer --train-mlp <trX> <trY> <Ntr> <teX> <teY> <Nte>"
           " <arch> <epochs> <lr> <mom> <batch> <seed> <out.rim|->\n");
    printf("       retro-infer --train-gbdt <feat> <lab> <N> <F> <valfrac>"
           " <rounds> <depth> <minchild> <lr> <lambda> [--regress]\n");
    printf("       retro-infer --train-rf <feat> <lab> <N> <F> <valfrac>"
           " <ntrees> <depth> <seed>\n");
    printf("       retro-infer --train-svm <feat> <lab> <N> <F> <valfrac>"
           " <epochs> <lr> <reg> [seed]\n");
    return 1;
}
