/*
 * retro-infer — dependency-free ML engine for the retro fleet.
 *
 * M0 CLI:
 *   retro-infer --selfcheck          ISA/RAM report + GFLOP/s microbench
 *   retro-infer --version
 *   retro-infer --riminfo <file>     validate a .rim and print its manifest
 *
 * Later: --infer, --train, --test-vectors, --serve 9897 (agent loopback).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "infer.h"

static void print_selfcheck(void)
{
    cpu_caps_t caps;
    MEMORYSTATUS ms;
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

    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    printf("ram.total_mb=%lu ram.avail_mb=%lu\n",
           (unsigned long)(ms.dwTotalPhys / (1024 * 1024)),
           (unsigned long)(ms.dwAvailPhys / (1024 * 1024)));

    printf("kernel.gemm_f32=%s kernel.gemm_i8=%s\n",
           g_kernels.gemm_f32_name, g_kernels.gemm_i8_name);

    printf("bench: gemm 256x256x256 fp32 ...\n");
    fflush(stdout);
    gflops_active = bench_gemm_gflops(256, 4);
    printf("bench.gflops.%s=%.3f\n", g_kernels.gemm_f32_name, gflops_active);

    if (strcmp(g_kernels.gemm_f32_name, "scalar") != 0) {
        saved = g_kernels;
        g_kernels.gemm_f32 = gemm_f32_scalar;
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

int main(int argc, char **argv)
{
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

    printf("retro-infer v%s\n", INFER_VERSION);
    printf("usage: retro-infer --selfcheck | --version | --riminfo <model.rim>\n");
    return 1;
}
