"""The per-box game resolutions must be applied on EVERY agent start (1.98.0).

THE DEFECT (.123, 2026-10-08)
-----------------------------
``gameres`` - detect the monitor, write every staged title's own config - had
exactly ONE caller: ``gs_run()``, at the end of each title's sync. And on a
provisioned box the startup GAMESYNC thread returns before it ever gets there::

    if (gs_file_exists(GS_MARKER)) { "already provisioned - idle"; return 0; }

That return is the NORMAL path on a fleet box. So a machine whose MONITOR was
changed kept every game at the old panel's resolution, across any number of
reboots, until somebody ran ``GAMERES APPLY`` or ``GAMESYNC RESET`` by hand -
which is precisely the failure the whole per-box resolution mechanism exists to
prevent. Nothing reported it: the configs were internally consistent, the sync
said ``state=done``, and the games asked the monitor for a mode it could not
show. ``.123`` was found with a CRT at 1280x1024 and Counter-Strike pinned at
1600x1200.

THE FIX
-------
``gameres_startup()`` runs from the gamesync startup thread **above** the
marker return, beside ``qbinds_startup()`` - the placement CLAUDE.md requires,
because anything below that return runs on almost no machine while looking
installed. A settled box writes 0.

What a unit test cannot see is WHERE in the startup thread this happens, which
is exactly what was broken - so that is what this file pins.
"""
import re
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GAMESYNC = (ROOT / "agent/src/gamesync.c").read_text(errors="replace")
GAMERES = (ROOT / "agent/src/gameres.c").read_text(errors="replace")
HANDLERS = (ROOT / "agent/src/handlers.h").read_text(errors="replace")


class TestGameresRunsAtStartup(unittest.TestCase):
    def test_the_startup_thread_calls_it(self):
        self.assertIn("gameres_startup();", GAMESYNC,
                      "the gamesync startup thread must call gameres_startup()")

    def test_it_is_called_ABOVE_the_already_provisioned_return(self):
        """The whole bug. Below that return it runs on almost no machine."""
        call = GAMESYNC.index("gameres_startup();")
        marker = GAMESYNC.index('if (gs_file_exists(GS_MARKER)) {')
        self.assertLess(
            call, marker,
            "gameres_startup() sits BELOW the gamesync.done marker return - "
            "that return is the normal path on a provisioned box, so the pass "
            "would never run on a fleet machine. This is the 1.98.0 defect.")

    def test_it_is_defined_and_declared(self):
        self.assertRegex(GAMERES, r"\bint\s+gameres_startup\(void\)\s*\n\{",
                         "gameres_startup() must be defined in gameres.c")
        self.assertIn("int  gameres_startup(void);", HANDLERS)

    def test_it_is_guarded_on_a_modern_host(self):
        body = GAMERES[GAMERES.index("int gameres_startup(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("host_policy_skip", body,
                      "a modern Windows box is NOT managed - the startup pass "
                      "must refuse there (CLAUDE.md, agent 1.82.0+)")

    def test_it_has_an_off_switch_and_records_its_result(self):
        self.assertIn('#define GR_REG_SWITCH "GameRes"', GAMERES)
        self.assertIn('#define GR_REG_BOOT   "GameResBoot"', GAMERES)
        body = GAMERES[GAMERES.index("int gameres_startup(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("gr_switch_on", body)
        self.assertIn("gr_store_boot", body)

    def test_it_never_falls_back_to_C(self):
        """GamesDir set-but-unusable must refuse, never write to C:\\Games."""
        body = GAMERES[GAMERES.index("int gameres_startup(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("gs_games_dir_why", body)
        self.assertIn("NOT APPLIED", body)

    def test_it_reports_the_post_condition_not_the_write_count(self):
        body = GAMERES[GAMERES.index("int gameres_startup(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("gr_verify_run", body,
                      "a log line saying we wrote the values is not evidence "
                      "the values are right - read them back")


class TestOneWalkerNotTwo(unittest.TestCase):
    """APPLY and the startup pass must walk the titles with the SAME code.

    Two copies disagree about which titles count as installed, after which
    "APPLY says 0 changed" and "the boot pass says 3 changed" are both true and
    neither is wrong.
    """

    def test_both_callers_use_gr_apply_all(self):
        self.assertRegex(GAMERES, r"static int gr_apply_all\(")
        n = len(re.findall(r"\bgr_apply_all\(", GAMERES))
        self.assertGreaterEqual(n, 3, "definition + both call sites expected")

    def test_the_walker_saves_the_ledger(self):
        """1.93.2: without this the next sync copies the library's files back."""
        body = GAMERES[GAMERES.index("static int gr_apply_all("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("gameres_ledger_save();", body)

    def test_the_apply_handler_no_longer_has_its_own_loop(self):
        blk = GAMERES[GAMERES.index('if (str_starts_with(a, "APPLY"))'):]
        blk = blk[:blk.index("        return;")]
        self.assertNotIn("GR_RULE_COUNT", blk,
                         "the APPLY handler kept a private copy of the walk")


if __name__ == "__main__":
    unittest.main()
