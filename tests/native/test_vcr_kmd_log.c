/* test_vcr_kmd_log.c
 *
 * The vcr-kmd flight recorder (voodoo-cleanroom/vcr-kmd/common/vcr_log.c) is
 * the kernel driver pair's "where did it fail" instrument: the miniport keeps
 * it in non-paged memory and tools/vcrdump.py finds it in a crash dump by its
 * magic. These tests pin the properties that finding and reading it rely on:
 * fixed geometry, the magic at offset 0, oldest-first reads across a wrap, and
 * that a record torn by a crash (seq not yet published) is never reported.
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_log.c"

#define N 8
static unsigned char ringmem[VCR_LOG_RING_BYTES(N)];
#define RING ((vcr_log_ring *)ringmem)

TEST(geometry_is_what_the_dump_parser_expects) {
    vcr_log_init(RING, N, 7, 0x00000100, 0x11111111, 0x22222222);
    CHECK_EQ_U(sizeof(vcr_log_entry), 128);
    CHECK_EQ_U(sizeof(vcr_log_header), 80);
    CHECK(memcmp(ringmem, "VCRKMD-FLIGHTREC", 16) == 0, "magic at offset 0");
    CHECK_EQ_U(RING->hdr.entry_size, 128);
    CHECK_EQ_U(RING->hdr.nentries, N);
    CHECK_EQ_U(RING->hdr.header_size, 80);
    CHECK_EQ_U(RING->hdr.boot_count, 7);
    /* the entries start right after the header, as vcrdump.py assumes */
    CHECK_EQ_U((unsigned char *)&RING->e[0] - ringmem, 80);
}

TEST(entries_come_back_in_order_with_their_fields) {
    vcr_log_entry out[N];
    vcr_u32 last = 0, n;
    vcr_log_init(RING, N, 0, 0, 0, 0);
    vcr_log_put(RING, 10, 101, VCR_SRC_MINIPORT, VCR_LV_INFO, 4, 1, 2, 3, 4, "find");
    vcr_log_put(RING, 20, 300, VCR_SRC_MINIPORT, VCR_LV_INFO, 0, 640, 480, 16, 60, "mode");
    vcr_log_put(RING, 30, 507, VCR_SRC_DISPLAY, VCR_LV_WARN, 99, 0x3df3, 0, 0, 0, NULL);
    n = vcr_log_read(RING, 0, out, N, &last);
    CHECK_EQ_U(n, 3);
    CHECK_EQ_U(last, 3);
    CHECK_EQ_U(out[0].seq, 1);
    CHECK_EQ_U(out[0].code, 101);
    CHECK_EQ_U(out[0].pid, 4);
    CHECK_EQ_U(out[0].d, 4);
    CHECK(strcmp(out[0].msg, "find") == 0, "text kept");
    CHECK_EQ_U(out[1].a, 640);
    CHECK_EQ_U(out[2].src, VCR_SRC_DISPLAY);
    CHECK_EQ_U(out[2].msg[0], 0);
}

TEST(a_wrapped_ring_returns_the_newest_entries_oldest_first) {
    vcr_log_entry out[N];
    vcr_u32 last = 0, n, i;
    vcr_log_init(RING, N, 0, 0, 0, 0);
    for (i = 1; i <= 20; i++)
        vcr_log_put(RING, i, 900, VCR_SRC_TOOL, VCR_LV_INFO, 0, i, 0, 0, 0, "x");
    n = vcr_log_read(RING, 0, out, N, &last);
    CHECK_EQ_U(n, N);
    CHECK_EQ_U(out[0].seq, 13);
    CHECK_EQ_U(out[0].a, 13);
    CHECK_EQ_U(out[N - 1].seq, 20);
    CHECK_EQ_U(last, 20);
}

TEST(after_seq_gives_only_what_is_new) {
    vcr_log_entry out[N];
    vcr_u32 last = 0, n, i;
    vcr_log_init(RING, N, 0, 0, 0, 0);
    for (i = 1; i <= 5; i++)
        vcr_log_put(RING, i, 900, VCR_SRC_TOOL, VCR_LV_INFO, 0, i, 0, 0, 0, "");
    n = vcr_log_read(RING, 3, out, N, &last);
    CHECK_EQ_U(n, 2);
    CHECK_EQ_U(out[0].seq, 4);
    n = vcr_log_read(RING, 5, out, N, &last);
    CHECK_EQ_U(n, 0);
    /* a reader limited to 1 entry at a time can page through */
    n = vcr_log_read(RING, 0, out, 1, &last);
    CHECK_EQ_U(n, 1);
    CHECK_EQ_U(last, 1);
}

TEST(a_record_torn_by_a_crash_is_not_reported) {
    vcr_log_entry out[N];
    vcr_u32 last = 0, n;
    vcr_log_init(RING, N, 0, 0, 0, 0);
    vcr_log_put(RING, 1, 1, VCR_SRC_TOOL, VCR_LV_INFO, 0, 0, 0, 0, 0, "a");
    vcr_log_put(RING, 2, 2, VCR_SRC_TOOL, VCR_LV_INFO, 0, 0, 0, 0, 0, "b");
    /* the crash hit between "seq = 0" and "publish seq" of entry 2 */
    RING->e[1].seq = 0;
    n = vcr_log_read(RING, 0, out, N, &last);
    CHECK_EQ_U(n, 1);
    CHECK_EQ_U(out[0].seq, 1);
}

TEST(overlong_text_is_truncated_and_terminated) {
    char big[300];
    vcr_log_entry out[1];
    vcr_u32 last;
    memset(big, 'A', sizeof big - 1);
    big[sizeof big - 1] = 0;
    vcr_log_init(RING, N, 0, 0, 0, 0);
    vcr_log_put(RING, 1, 0, VCR_SRC_TOOL, VCR_LV_INFO, 0, 0, 0, 0, 0, big);
    vcr_log_read(RING, 0, out, 1, &last);
    CHECK_EQ_U(strlen(out[0].msg), VCR_LOG_MSG_LEN - 1);
    /* a neighbouring entry was not overrun */
    CHECK_EQ_U(RING->e[1].seq, 0);
}

MUNIT_MAIN("vcr-kmd flight recorder", {
    RUN(geometry_is_what_the_dump_parser_expects);
    RUN(entries_come_back_in_order_with_their_fields);
    RUN(a_wrapped_ring_returns_the_newest_entries_oldest_first);
    RUN(after_seq_gives_only_what_is_new);
    RUN(a_record_torn_by_a_crash_is_not_reported);
    RUN(overlong_text_is_truncated_and_terminated);
})
