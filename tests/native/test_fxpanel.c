/* test_fxpanel.c - TRUE-SOURCE: compiles the REAL agent/shared/fxpanel.h (which
 * boxes get the 3dfx Control Panel, and when its copy is refreshed) and the
 * lnkcheck.h icon matcher the desktop shortcut uses (agent 1.94.0).
 *
 * THE OLD BEHAVIOUR: the panel reached a box only when someone ran
 * push_3dfxctl.py, and its desktop shortcut (the old triangle icon) was swept
 * off by the next GAMESYNC. The user asked for the 3dfx logo on the desktop of
 * every box with an applicable 3dfx card, deployed by the agent when it loads.
 */
#include "munit.h"
#include <string.h>

#include "../../agent/shared/fxpanel.h"
#include "../../agent/shared/lnkcheck.h"

TEST(only_the_cards_the_panel_serves)
{
    CHECK_EQ_I(fxpanel_dev_has_panel(0x0003), 1);   /* Banshee */
    CHECK_EQ_I(fxpanel_dev_has_panel(0x0004), 1);   /* Banshee */
    CHECK_EQ_I(fxpanel_dev_has_panel(0x0005), 1);   /* Voodoo3 */
    CHECK_EQ_I(fxpanel_dev_has_panel(0x0009), 1);   /* VSA-100: V4, V5 5500/6000 */
    CHECK_EQ_I(fxpanel_dev_has_panel(0x0001), 0);   /* Voodoo Graphics: 3D-only */
    CHECK_EQ_I(fxpanel_dev_has_panel(0x0002), 0);   /* Voodoo2 (.243): 3D-only */
    CHECK_EQ_I(fxpanel_dev_has_panel(0x0000), 0);
}

TEST(only_windows_2000_and_xp)
{
    CHECK_EQ_I(fxpanel_os_ok(1, 5), 1);             /* XP / 2000 / 2003 */
    CHECK_EQ_I(fxpanel_os_ok(1, 6), 0);             /* Windows 7 (.195): no 3dfx driver */
    CHECK_EQ_I(fxpanel_os_ok(1, 10), 0);
    CHECK_EQ_I(fxpanel_os_ok(0, 4), 0);             /* Windows 98 */
}

TEST(the_decision_order)
{
    CHECK_EQ_I(fxpanel_decide(1, 1, 5, 1), FXP_OFF);        /* the switch wins */
    CHECK_EQ_I(fxpanel_decide(0, 0, 4, 1), FXP_NOT_OS);
    CHECK_EQ_I(fxpanel_decide(0, 1, 6, 1), FXP_NOT_OS);
    CHECK_EQ_I(fxpanel_decide(0, 1, 5, 0), FXP_NO_CARD);
    CHECK_EQ_I(fxpanel_decide(0, 1, 5, 1), FXP_DEPLOY);     /* .124: XP + V5 6000 */
}

TEST(missing_or_different_is_copied_same_is_left_alone)
{
    unsigned long long t = 134000000000000000ULL;
    CHECK_EQ_I(fxpanel_need_copy(0, 0, 0, 427434, t), 1);          /* missing */
    CHECK_EQ_I(fxpanel_need_copy(1, 427434, t, 427434, t), 0);     /* settled box */
    CHECK_EQ_I(fxpanel_need_copy(1, 427434, t, 430000, t), 1);     /* new build */
    CHECK_EQ_I(fxpanel_need_copy(1, 427434, t, 427434, t + 20000001ULL), 1);
    /* FAT keeps 2-second times: within 2 s is the same file, either way */
    CHECK_EQ_I(fxpanel_need_copy(1, 427434, t + 19999999ULL, 427434, t), 0);
    CHECK_EQ_I(fxpanel_need_copy(1, 427434, t, 427434, t + 19999999ULL), 0);
}

/* A shell-link StringData entry: 16-bit count + UTF-16LE characters. */
static size_t put_counted(unsigned char *out, const char *s)
{
    size_t n = strlen(s), i, k = 0;
    out[k++] = (unsigned char)(n & 0xFF);
    out[k++] = (unsigned char)(n >> 8);
    for (i = 0; i < n; i++) { out[k++] = (unsigned char)s[i]; out[k++] = 0; }
    return k;
}

#define LOGO "C:\\RETRO_AGENT\\3dfxlogo.ico"

TEST(the_shortcut_names_the_logo_or_is_rewritten)
{
    unsigned char b[512];
    size_t n;
    memset(b, 0xCC, sizeof(b));
    n = 40 + put_counted(b + 40, LOGO);
    CHECK_EQ_I(lnk_bytes_counted_wstr(b, n, LOGO), 1);
    CHECK_EQ_I(lnk_bytes_counted_wstr(b, n, "c:\\retro_agent\\3DFXLOGO.ICO"), 1);
    /* the pushed shortcut before 1.94.0 named the exe: rewritten */
    n = 40 + put_counted(b + 40, "C:\\RETRO_AGENT\\3dfxctl.exe");
    CHECK_EQ_I(lnk_bytes_counted_wstr(b, n, LOGO), 0);
    /* a longer string that starts the same is not a match (the count differs) */
    n = 40 + put_counted(b + 40, LOGO ".bak");
    CHECK_EQ_I(lnk_bytes_counted_wstr(b, n, LOGO), 0);
    /* truncated buffer, empty needle */
    n = 40 + put_counted(b + 40, LOGO);
    CHECK_EQ_I(lnk_bytes_counted_wstr(b, n - 2, LOGO), 0);
    CHECK_EQ_I(lnk_bytes_counted_wstr(b, n, ""), 0);
}

MUNIT_MAIN("3dfx Control Panel deploy (agent/shared/fxpanel.h, agent 1.94.0)",
    RUN(only_the_cards_the_panel_serves);
    RUN(only_windows_2000_and_xp);
    RUN(the_decision_order);
    RUN(missing_or_different_is_copied_same_is_left_alone);
    RUN(the_shortcut_names_the_logo_or_is_rewritten);
)
