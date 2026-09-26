/* agent/shared/clockguard.h - may clockfix.c move this box's clock? (agent 1.85.2)
 *
 * 2026-09-26: a Dell Dimension 4600 was PXE-imaged with its CMOS clock at
 * 2004-02-21, so XP recorded the install - and began its 30-day activation
 * grace - in 2004. At first logon clockfix set the clock to 2026-09-26 and WMI
 * then read ActivationRequired=1, RemainingGracePeriod=0: the next reboot would
 * have stopped at XP's logon-blocking activation screen, where the agent never
 * starts. Before 1.85.2 clockfix moved the clock unconditionally (the "old"
 * verdict below); now clockfix_thread -> clk_activation_allows asks Windows. */
#include <stdio.h>
#include "../../agent/shared/clockguard.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

/* What 1.85.1 did: always move. */
static int old_verdict(void) { return CLOCKGUARD_ALLOW; }

int main(void)
{
    /* The Dell, measured: 2004-02-21 -> 2026-09-26 is 8253 days; grace 30
     * before the move (the box had just been installed, in 2004). */
    const long long dell_jump = 8253;

    CHECK(clockguard_decide(1, 1, 1, 30, dell_jump) == CLOCKGUARD_REFUSE_UNACTIVATED,
          "the Dell: unactivated, 30 days of grace, a 22-year jump -> REFUSED");
    CHECK(old_verdict() == CLOCKGUARD_ALLOW,
          "(1.85.1 moved it anyway - which is what spent the grace)");
    CHECK(clockguard_decide(1, 1, 0, 2147483647UL, dell_jump) == CLOCKGUARD_ALLOW,
          "the same box once activated -> allowed (next agent start fixes the clock)");
    CHECK(clockguard_decide(1, 1, 1, 0, dell_jump) == CLOCKGUARD_REFUSE_UNACTIVATED,
          "grace already 0 -> refused (moving it cannot make it worse, but must not look fixed)");
    CHECK(clockguard_decide(1, 0, 0, 0, dell_jump) == CLOCKGUARD_REFUSE_UNKNOWN,
          "XP with WMI unreadable -> refused: a wrong clock is fixable later, a locked box is not");
    CHECK(clockguard_decide(1, 1, 1, 9000, dell_jump) == CLOCKGUARD_ALLOW,
          "installed with a RIGHT clock, battery reset it to 2004 at a cold boot: grace reads "
          "huge, the move restores the truth -> allowed");
    CHECK(clockguard_decide(1, 1, 1, 10, 9) == CLOCKGUARD_REFUSE_UNACTIVATED,
          "grace 10, jump 9: inside the one-day margin -> refused");
    CHECK(clockguard_decide(1, 1, 1, 11, 9) == CLOCKGUARD_ALLOW,
          "grace 11, jump 9 -> allowed");
    CHECK(clockguard_decide(1, 1, 1, 5, -400) == CLOCKGUARD_ALLOW,
          "moving the clock BACK only adds grace -> allowed");
    CHECK(clockguard_decide(0, 0, 0, 0, dell_jump) == CLOCKGUARD_ALLOW,
          "not XP/2003 (9x, 2000, Vista+) -> allowed, no WMI needed");
    CHECK(clockguard_wpa_os(1, 5, 1) && clockguard_wpa_os(1, 5, 2),
          "XP (5.1) and Server 2003 / XP x64 (5.2) are the logon-blocking versions");
    CHECK(!clockguard_wpa_os(1, 5, 0) && !clockguard_wpa_os(1, 6, 1) && !clockguard_wpa_os(0, 4, 10),
          "2000, Windows 7 and Win98 are not");
    printf("-- clockguard (clockfix vs XP activation, agent 1.85.2): %d/%d tests passed --\n",
           runs - fails, runs);
    return fails ? 1 : 0;
}
