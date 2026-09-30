/* test_vcr_aaguard.c - TRUE-SOURCE: compiles the REAL
 * voodoo-cleanroom/vcr-kmd/include/vcr_aaguard.h, the AA auto-disarm after a
 * freeze (2026-09-30).
 *
 * THE OLD BEHAVIOUR: Quake II at SSTH3_SLI_AA_CONFIGURATION = 6 froze .124,
 * AA stayed armed, and the next boot froze too the moment a game started
 * (vcrphases --prev: SLI SET_DONE at 130 s of boot #46). Nothing disarmed it.
 */
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_aaguard.h"

TEST(glide_aa_values_match_h5sliaa)
{
    unsigned aa[] = { 1, 3, 4, 6, 7, 8 }, plain[] = { 0, 2, 5, 9, 10, 255 };
    unsigned i;
    for (i = 0; i < 6; i++) CHECK_EQ_I(vcr_aag_cfg_is_aa(aa[i]), 1);
    for (i = 0; i < 6; i++) CHECK_EQ_I(vcr_aag_cfg_is_aa(plain[i]), 0);
}

TEST(the_safe_value_is_plain_sli_for_the_board)
{
    CHECK_EQ_I(vcr_aag_safe_cfg(4), 5);   /* V5 6000 */
    CHECK_EQ_I(vcr_aag_safe_cfg(2), 2);   /* V5 5500 */
    CHECK_EQ_I(vcr_aag_safe_cfg(1), 0);   /* V4 */
    CHECK_EQ_I(vcr_aag_cfg_is_aa(vcr_aag_safe_cfg(4)), 0);
    CHECK_EQ_I(vcr_aag_cfg_is_aa(vcr_aag_safe_cfg(2)), 0);
    CHECK_EQ_I(vcr_aag_cfg_is_aa(vcr_aag_safe_cfg(1)), 0);
}

TEST(the_reg_sz_parse_never_guesses)
{
    unsigned short six[] = { '6', 0 }, sp[] = { ' ', '7' }, bad[] = { '6', 'x' },
                   empty[] = { 0 }, big[] = { '1', '2', '3', '4' };
    CHECK_EQ_I(vcr_aag_parse(six, 2), 6);
    CHECK_EQ_I(vcr_aag_parse(sp, 2), 7);
    CHECK_EQ_I(vcr_aag_parse(bad, 2), -1);
    CHECK_EQ_I(vcr_aag_parse(empty, 1), -1);
    CHECK_EQ_I(vcr_aag_parse(big, 4), -1);
}

TEST(only_a_live_marker_disarms)
{
    CHECK_EQ_I(vcr_aag_boot(1), VCR_AAG_DISARM);
    CHECK_EQ_I(vcr_aag_boot(0), VCR_AAG_NOTHING);
    CHECK_EQ_I(vcr_aag_boot(0xffffffffUL), VCR_AAG_NOTHING);
}

MUNIT_MAIN("AA auto-disarm (vcr-kmd include/vcr_aaguard.h, 2026-09-30)",
    RUN(glide_aa_values_match_h5sliaa);
    RUN(the_safe_value_is_plain_sli_for_the_board);
    RUN(the_reg_sz_parse_never_guesses);
    RUN(only_a_live_marker_disarms);
)
