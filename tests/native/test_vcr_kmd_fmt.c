/* test_vcr_kmd_fmt.c
 *
 * vcr_vsnprintf (voodoo-cleanroom/vcr-kmd/common/vcr_fmt.c) formats every log
 * line the kernel driver pair writes: the display DLL may import only
 * win32k.sys (no vsnprintf there) and the miniport logs at raised IRQL. A
 * formatter bug there would garble the one record that says where the driver
 * failed, so pin the conversions the driver uses and the truncation contract.
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_fmt.c"

static char b[64];

TEST(integer_conversions) {
    vcr_snprintf(b, sizeof b, "%d %i %u", -42, 7, 4000000000u);
    CHECK(strcmp(b, "-42 7 4000000000") == 0, b);
    vcr_snprintf(b, sizeof b, "%x %X %08x", 0xbeefu, 0xbeefu, 0x121au);
    CHECK(strcmp(b, "beef BEEF 0000121a") == 0, b);
    vcr_snprintf(b, sizeof b, "%d", (int)0x80000000);
    CHECK(strcmp(b, "-2147483648") == 0, b);
    vcr_snprintf(b, sizeof b, "%lx %lu %hu", 0x10ul, 5ul, 3);
    CHECK(strcmp(b, "10 5 3") == 0, b);
}

TEST(widths_strings_and_chars) {
    vcr_snprintf(b, sizeof b, "[%5d][%-5d][%05d]", 42, 42, -42);
    CHECK(strcmp(b, "[   42][42   ][-0042]") == 0, b);
    vcr_snprintf(b, sizeof b, "%s|%6s|%-6s|%c", "ab", "ab", "ab", 'Z');
    CHECK(strcmp(b, "ab|    ab|ab    |Z") == 0, b);
    vcr_snprintf(b, sizeof b, "%s %% %q", (char *)0);
    CHECK(strcmp(b, "(null) % %q") == 0, b);
}

TEST(pointers_are_eight_hex_digits) {
    vcr_snprintf(b, sizeof b, "%p", (void *)(unsigned long)0x1234);
    CHECK(strcmp(b, "00001234") == 0, b);
}

TEST(output_is_truncated_and_always_terminated) {
    char s[8];
    int n = vcr_snprintf(s, sizeof s, "%s", "0123456789");
    CHECK(strcmp(s, "0123456") == 0, s);
    CHECK_EQ_I(n, 10);      /* the would-be length, like C99 snprintf */
    memset(s, 'x', sizeof s);
    vcr_snprintf(s, 1, "abc");
    CHECK_EQ_U(s[0], 0);
}

MUNIT_MAIN("vcr-kmd formatter", {
    RUN(integer_conversions);
    RUN(widths_strings_and_chars);
    RUN(pointers_are_eight_hex_digits);
    RUN(output_is_truncated_and_always_terminated);
})
