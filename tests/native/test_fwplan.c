/* test_fwplan.c - TRUE-SOURCE: compiles the REAL agent/shared/fwplan.h, which
 * decides whether the agent runs netsh to open the Windows Firewall for itself
 * (agent 1.85.0).
 *
 * THE OLD-BUGGY BEHAVIOUR (main.c ensure_firewall_exception, <= 1.84.x): on
 * every start, before listen(), run `netsh firewall ...` AND
 * `netsh advfirewall ...` (up to 5 s each), on every OS, with no check that the
 * exception already existed - and log "added" whenever netsh merely started.
 * On Win9x there is no netsh; on XP there is no advfirewall context, so that
 * half failed on every boot. old_plan() models it for contrast.
 */
#include "munit.h"
#include <string.h>

#include "../../agent/shared/fwplan.h"

static int old_plan(void) { return FWP_NETSH_FIREWALL | FWP_NETSH_ADV; }

#define EXE "C:\\RETRO_AGENT\\retro_agent.exe"

TEST(win9x_and_win2000_have_no_firewall_to_open)
{
    CHECK_EQ_I(fw_plan(0, 4, 10, 0), 0);      /* Win98SE: 4.10, not NT */
    CHECK_EQ_I(fw_plan(0, 4, 90, 0), 0);      /* WinME */
    CHECK_EQ_I(fw_plan(1, 4, 0, 0), 0);       /* NT4 */
    CHECK_EQ_I(fw_plan(1, 5, 0, 0), 0);       /* Windows 2000 */
    CHECK_EQ_I(old_plan(), FWP_NETSH_FIREWALL | FWP_NETSH_ADV);  /* ran anyway */
}

TEST(xp_runs_only_the_context_it_has)
{
    CHECK_EQ_I(fw_plan(1, 5, 1, 0), FWP_NETSH_FIREWALL);  /* XP */
    CHECK_EQ_I(fw_plan(1, 5, 2, 0), FWP_NETSH_FIREWALL);  /* 2003 / XP x64 */
    CHECK((old_plan() & FWP_NETSH_ADV) != 0, "the old code ran advfirewall on XP");
}

TEST(vista_and_later_run_both)
{
    CHECK_EQ_I(fw_plan(1, 6, 1, 0), FWP_NETSH_FIREWALL | FWP_NETSH_ADV);  /* Win7 */
    /* 10/11 report 6.2 to an unmanifested exe (the GetVersionEx shim) */
    CHECK_EQ_I(fw_plan(1, 6, 2, 0), FWP_NETSH_FIREWALL | FWP_NETSH_ADV);
}

TEST(an_existing_exception_means_no_netsh_at_all)
{
    CHECK_EQ_I(fw_plan(1, 5, 1, 1), 0);
    CHECK_EQ_I(fw_plan(1, 6, 1, 1), 0);
}

TEST(the_xp_list_entry_is_parsed_past_the_drive_colon)
{
    CHECK_EQ_I(fw_list_entry_enabled(EXE ":*:Enabled:Retro Agent", EXE), 1);
    /* case differs: Windows paths are case-insensitive */
    CHECK_EQ_I(fw_list_entry_enabled("c:\\retro_agent\\RETRO_AGENT.EXE:*:Enabled:x",
                                     EXE), 1);
    CHECK_EQ_I(fw_list_entry_enabled(EXE ":LocalSubNet:Enabled:Retro Agent", EXE), 1);
    /* a disabled entry is not an exception */
    CHECK_EQ_I(fw_list_entry_enabled(EXE ":*:Disabled:Retro Agent", EXE), 0);
    /* a different program whose path merely starts the same */
    CHECK_EQ_I(fw_list_entry_enabled("C:\\RETRO_AGENT\\retro_agent.exe.old:*:Enabled:x",
                                     EXE), 0);
    CHECK_EQ_I(fw_list_entry_enabled("D:\\RETRO_AGENT\\retro_agent.exe:*:Enabled:x",
                                     EXE), 0);
    CHECK_EQ_I(fw_list_entry_enabled("", EXE), 0);
    CHECK_EQ_I(fw_list_entry_enabled(EXE, EXE), 0);        /* truncated data */
}

TEST(a_vista_rule_must_be_an_active_inbound_allow_for_this_exe)
{
    const char *ok =
        "v2.10|Action=Allow|Active=TRUE|Dir=In|Protocol=6|LPort=9898|"
        "App=" EXE "|Name=Retro Agent|";
    /* what `netsh firewall add allowedprogram` writes on Win7 (no port) */
    const char *legacy =
        "v2.10|Action=Allow|Active=TRUE|Dir=In|Profile=Private|"
        "App=c:\\retro_agent\\retro_agent.exe|Name=Retro Agent|";
    CHECK_EQ_I(fw_rule_allows(ok, EXE), 1);
    CHECK_EQ_I(fw_rule_allows(legacy, EXE), 1);
    CHECK_EQ_I(fw_rule_allows(
        "v2.10|Action=Block|Active=TRUE|Dir=In|App=" EXE "|Name=x|", EXE), 0);
    CHECK_EQ_I(fw_rule_allows(
        "v2.10|Action=Allow|Active=FALSE|Dir=In|App=" EXE "|Name=x|", EXE), 0);
    CHECK_EQ_I(fw_rule_allows(
        "v2.10|Action=Allow|Active=TRUE|Dir=Out|App=" EXE "|Name=x|", EXE), 0);
    CHECK_EQ_I(fw_rule_allows(
        "v2.10|Action=Allow|Active=TRUE|Dir=In|App=" EXE ".old|Name=x|", EXE), 0);
    CHECK_EQ_I(fw_rule_allows(
        "v2.10|Action=Allow|Active=TRUE|Dir=In|App=C:\\x\\hl.exe|Name=x|", EXE), 0);
}

MUNIT_MAIN("firewall step (agent/shared/fwplan.h, agent 1.85.0)",
    RUN(win9x_and_win2000_have_no_firewall_to_open);
    RUN(xp_runs_only_the_context_it_has);
    RUN(vista_and_later_run_both);
    RUN(an_existing_exception_means_no_netsh_at_all);
    RUN(the_xp_list_entry_is_parsed_past_the_drive_colon);
    RUN(a_vista_rule_must_be_an_active_inbound_allow_for_this_exe);
)
