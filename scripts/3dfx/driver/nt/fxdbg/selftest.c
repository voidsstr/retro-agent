/*
 * selftest.c - host-side ABI guard for the M4d bring-up ladder (fxdbg <-> driver).
 *
 * fxdbg.exe (user tool) and gbkdebug.c (in the display driver) share ONE ABI:
 * the fixed structs and opcodes in gbkdebug.h. If either side's layout drifts,
 * ExtEscape returns garbage on-card and the ladder silently mis-reports. This
 * test pins the wire layout so a stray field edit fails the build, not the box.
 *
 * Built with -m32 so `unsigned long` is 4 bytes (ILP32), matching the i386
 * display-driver target the escapes actually run on - the same gate the
 * gbkernel-test suite uses. No hardware, no Win32 headers (gbkdebug.h is
 * DDK-agnostic by design).
 */
#include <stdio.h>
#include "../gbkdebug.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } \
} while (0)

#define EQ(got, want, msg) do { \
    unsigned long g_ = (unsigned long)(got), w_ = (unsigned long)(want); \
    if (g_ != w_) { \
        printf("FAIL: %s: got %lu want %lu\n", msg, g_, w_); ++fails; \
    } \
} while (0)

int
main(void)
{
    /* ILP32 gate: the on-card ABI assumes 4-byte unsigned long. */
    EQ(sizeof(unsigned long), 4, "unsigned long is 32-bit (build with -m32)");

    /* Opcode namespace (private 0x3DF0.. range). */
    EQ(FXDBG_ESC_BASE, 0x3DF0, "FXDBG_ESC_BASE");
    EQ(FXDBG_PROBE,    0x3DF0, "FXDBG_PROBE");
    EQ(FXDBG_CLEAR,    0x3DF1, "FXDBG_CLEAR");
    EQ(FXDBG_TRI,      0x3DF2, "FXDBG_TRI");
    EQ(FXDBG_TEX,      0x3DF3, "FXDBG_TEX");
    EQ(FXDBG_READBACK, 0x3DF4, "FXDBG_READBACK");
    EQ(FXDBG_MAGIC,    0x46584442, "FXDBG_MAGIC == 'FXDB'");

    EQ(FXDBG_RB_BACK,    0, "FXDBG_RB_BACK");
    EQ(FXDBG_RB_DESKTOP, 1, "FXDBG_RB_DESKTOP");

    /* Wire struct sizes (must match what gbkdebug.c's guards check). */
    EQ(sizeof(fxdbg_probe_t),     28u * 4u, "sizeof(fxdbg_probe_t) = 28 ULONGs");
    EQ(sizeof(fxdbg_status_out_t), 3u * 4u, "sizeof(fxdbg_status_out_t) = 3 ULONGs");
    EQ(sizeof(fxdbg_clear_in_t),   3u * 4u, "sizeof(fxdbg_clear_in_t) = 3 ULONGs");
    EQ(sizeof(fxdbg_readback_in_t),5u * 4u, "sizeof(fxdbg_readback_in_t) = 5 ULONGs");

    /* First/last field offsets - catches a field inserted mid-struct. */
    {
        fxdbg_probe_t p;
        CHECK((char *)&p.magic   == (char *)&p,          "probe.magic at offset 0");
        EQ((char *)&p.swReadOfs - (char *)&p, 27u * 4u,  "probe.swReadOfs last field");
    }

    if (fails == 0)
        printf("fxdbg selftest: ALL PASS (ABI pinned)\n");
    else
        printf("fxdbg selftest: %d FAILURE(S)\n", fails);
    return fails ? 1 : 0;
}
