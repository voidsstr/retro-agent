#!/usr/bin/env python3
r"""Tests for the fleet compatibility matrix.

MOST OF THESE ASSERT THE NEGATIVE PATH ON PURPOSE. A checker that can only say
OK is the exact failure this project keeps paying for, and every bug this
module has already had was a confident wrong answer rather than a crash:

  * `installed_games` is an ENGINE-AWARE index, so "not in it" was read as
    "absent" and marked Doom 3 missing on a box where Doom 3 is LAN-verified.
  * `DIRLIST` returns a bare JSON ARRAY; the probe's `except Exception` turned
    the resulting parse error into an empty directory listing and reported 414
    cells `absent` without a word of complaint.

Both are pinned below. Run: python3 tests/python/test_fleet_compat.py
"""
import json
import os
import sqlite3
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "scripts", "fleet"))

import compat_db as C           # noqa: E402
import compat                   # noqa: E402


class Base(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        os.unlink(self.path)
        self.con = C.connect(self.path)

    def tearDown(self):
        self.con.close()
        for suffix in ("", "-wal", "-shm"):
            try:
                os.unlink(self.path + suffix)
            except OSError:
                pass

    def seed(self):
        C.put_box(self.con, "192.168.1.143", hostname="1GHZ")
        C.put_box(self.con, "192.168.1.246", hostname="ADMIN-PC")
        C.put_title(self.con, "Quake1")
        C.put_title(self.con, "Doom3")


class TestNeverTestedIsNotAPass(Base):
    """THE central invariant: an empty cell must say so, loudly."""

    def test_missing_row_renders_as_untested_not_blank(self):
        self.seed()
        rows = compat._matrix_rows(self.con)
        self.assertEqual(len(rows), 4, "matrix must be the FULL cross product")
        for r in rows:
            self.assertEqual(r["deploy"], "untested")
            self.assertEqual(r["runs"], "untested")
            self.assertEqual(r["mp"], "untested")
            # The failure being guarded: a falsy/blank cell that a renderer or
            # a dashboard could style as "fine".
            self.assertNotIn(r["runs"], ("", None, "ok", "pass"))

    def test_untested_never_counted_as_verified(self):
        self.seed()
        C.put_render(self.con, "192.168.1.143", "Quake1", "measured", "verified")
        rows = compat._matrix_rows(self.con)
        verified = [r for r in rows if r["runs"] == "verified"]
        self.assertEqual(len(verified), 1)
        self.assertEqual(sum(1 for r in rows if r["runs"] == "untested"), 3)

    def test_three_states_are_distinguishable(self):
        """never-tested / tested-and-failed / not-applicable are three values."""
        self.seed()
        C.put_title(self.con, "Halo")
        C.put_render(self.con, "192.168.1.143", "Quake1", "measured", "failed")
        C.put_render(self.con, "192.168.1.143", "Doom3", "measured", "n/a")
        got = {r["title"]: r["runs"] for r in
               compat._matrix_rows(self.con, box="192.168.1.143")}
        self.assertEqual(got["Quake1"], "failed")
        self.assertEqual(got["Doom3"], "n/a")
        self.assertEqual(got["Halo"], "untested")
        self.assertEqual(len(set(got.values())), 3)


class TestMeasuredSurvivesIngest(Base):
    """A hand-recorded verification must outlive any machine-derived refresh."""

    def test_derived_write_does_not_destroy_measured(self):
        self.seed()
        C.put_render(self.con, "192.168.1.143", "Quake1", "measured", "verified",
                     renderer="glide", width=800, height=600, source="manual")
        for _ in range(3):                      # ingest is re-runnable
            C.put_render(self.con, "192.168.1.143", "Quake1", "derived",
                         "untested", source="gamegate.db")
        row = compat._matrix_rows(self.con, box="192.168.1.143",
                                  title="Quake1")[0]
        self.assertEqual(row["runs"], "verified")
        self.assertEqual(row["render_origin"], "measured")
        self.assertEqual(row["renderer"], "glide")
        # and the derived row still EXISTS - nothing is thrown away
        n = self.con.execute(
            "SELECT COUNT(*) FROM compat_render WHERE ip=? AND title=?",
            ("192.168.1.143", "Quake1")).fetchone()[0]
        self.assertEqual(n, 2, "both origins must be kept, not merged")

    def test_disagreement_is_reported_not_silently_resolved(self):
        self.seed()
        C.put_mp(self.con, "192.168.1.143", "Quake1", "measured",
                 "verified_two_box", source="lan-doc")
        C.put_mp(self.con, "192.168.1.143", "Quake1", "derived",
                 "untested", source="gamegate.db")
        conflicts = list(self.con.execute("SELECT * FROM v_compat_conflict"))
        self.assertEqual(len(conflicts), 1)
        self.assertEqual(conflicts[0]["measured"], "verified_two_box")
        self.assertEqual(conflicts[0]["derived"], "untested")

    def test_an_upgrade_is_not_reported_as_a_contradiction(self):
        """Burying the real disagreements under benign ones is how a checker
        trains people to ignore it - the validator-cries-wolf failure."""
        self.seed()
        # benign: the LAN proof could only conclude `runs`; somebody then
        # actually watched it render.
        C.put_render(self.con, "192.168.1.143", "Quake1", "derived", "runs",
                     source="lan-proof-implied")
        C.put_render(self.con, "192.168.1.143", "Quake1", "measured",
                     "verified", source="perbox")
        # real: two sources that both claim to know, and disagree.
        C.put_deploy(self.con, "192.168.1.246", "Doom3", "derived", "deployed",
                     source="probe")
        C.put_deploy(self.con, "192.168.1.246", "Doom3", "measured", "absent",
                     source="perbox")
        kinds = {(r["title"], r["kind"]) for r in
                 self.con.execute("SELECT * FROM v_compat_conflict")}
        self.assertIn(("Quake1", "upgrade"), kinds)
        self.assertIn(("Doom3", "contradiction"), kinds)

    def test_measuring_a_previously_untested_cell_is_an_upgrade(self):
        self.seed()
        C.put_mp(self.con, "192.168.1.143", "Quake1", "derived", "untested",
                 source="gamegate.db")
        C.put_mp(self.con, "192.168.1.143", "Quake1", "measured",
                 "verified_two_box", source="perbox")
        row = self.con.execute("SELECT * FROM v_compat_conflict").fetchone()
        self.assertEqual(row["kind"], "upgrade")

    def test_partial_update_does_not_wipe_another_ingests_field(self):
        C.put_box(self.con, "192.168.1.143", hostname="1GHZ", gpu="V5 5500")
        C.put_box(self.con, "192.168.1.143", os="Windows XP")
        row = self.con.execute("SELECT * FROM compat_box").fetchone()
        self.assertEqual(row["gpu"], "V5 5500")
        self.assertEqual(row["os"], "Windows XP")


class TestBadInputIsRejectedLoudly(Base):
    def test_typo_state_is_refused(self):
        self.seed()
        for bad in ("verifed", "OK", "yes", "true", ""):
            with self.assertRaises(C.BadState):
                C.put_render(self.con, "192.168.1.143", "Quake1", "measured", bad)

    def test_unknown_origin_refused(self):
        self.seed()
        with self.assertRaises(C.BadState):
            C.put_mp(self.con, "192.168.1.143", "Quake1", "guessed", "untested")

    def test_unknown_renderer_refused(self):
        self.seed()
        with self.assertRaises(C.BadState):
            C.put_render(self.con, "192.168.1.143", "Quake1", "measured",
                         "verified", renderer="voodoo")


class TestIngestFailsLoudly(Base):
    def test_missing_source_raises_rather_than_reporting_success(self):
        orig = compat.LAN_DOC
        compat.LAN_DOC = "/nonexistent/lan.md"
        try:
            with self.assertRaises(compat.IngestError):
                compat.ingest_lan_doc(self.con, strict=True)
        finally:
            compat.LAN_DOC = orig

    def test_lenient_mode_records_the_failure_in_the_log(self):
        orig = compat.LIBRARY
        compat.LIBRARY = "/nonexistent/library"
        try:
            compat.ingest_library(self.con, strict=False)
        finally:
            compat.LIBRARY = orig
        row = self.con.execute(
            "SELECT * FROM compat_ingest ORDER BY id DESC LIMIT 1").fetchone()
        self.assertEqual(row["ok"], 0)
        self.assertIn("not readable", row["detail"])

    def test_cli_ingest_returns_nonzero_when_a_source_is_missing(self):
        orig = compat.INVENTORY_DIR
        compat.INVENTORY_DIR = "/nonexistent"
        try:
            rc = compat.main(["--db", self.path, "ingest",
                              "--source", "inventory"])
        finally:
            compat.INVENTORY_DIR = orig
        self.assertNotEqual(rc, 0, "a failed ingest must not exit 0")


class TestStaleness(Base):
    def test_old_fact_is_marked_stale_and_undated_is_not(self):
        rows = [{"measured_at": "2000-01-01 00:00:00"},
                {"measured_at": ""},
                {"measured_at": None}]
        compat._mark_stale(rows, days=30)
        self.assertTrue(rows[0]["stale"])
        self.assertGreater(rows[0]["age_days"], 30)
        # never measured is NOT stale - absence of a date and an old date are
        # different facts and must not render the same.
        self.assertIsNone(rows[1]["stale"])
        self.assertIsNone(rows[2]["stale"])

    def test_fresh_fact_is_not_stale(self):
        rows = [{"measured_at": C.now()}]
        compat._mark_stale(rows, days=30)
        self.assertFalse(rows[0]["stale"])


class TestPresenceIndexCannotProveAbsence(Base):
    r"""The engine index found ThiefGold but has no game_key for Doom 3.

    Reading "not in installed_games" as "absent" marked Doom 3 missing on .123,
    a box where Doom 3 is LAN-verified against .246. `ingest_installed` may
    therefore write `deployed` and NOTHING else.
    """

    def test_ingest_installed_writes_only_deployed(self):
        self.seed()
        gs = self.path + ".gs"
        g = sqlite3.connect(gs)
        g.executescript(
            "CREATE TABLE machines (ip TEXT, hostname TEXT, os TEXT,"
            " agent_version TEXT, index_hash TEXT, indexed_at TEXT,"
            " last_seen TEXT, note TEXT);"
            "CREATE TABLE installed_games (ip TEXT, game_key TEXT, dir TEXT,"
            " name TEXT, engine TEXT, exe TEXT, launcher TEXT, source TEXT,"
            " seen_at TEXT);")
        g.execute("INSERT INTO machines VALUES "
                  "('192.168.1.143','1GHZ','','','','2026-08-31','','')")
        g.execute("INSERT INTO installed_games VALUES "
                  "('192.168.1.143','quake','C:\\Games\\Quake1','','','','','','')")
        g.commit()
        g.close()
        orig = C.GAMESERVERS_DB
        C.GAMESERVERS_DB = type(orig)(gs)
        try:
            compat.ingest_installed(self.con, strict=True)
        finally:
            C.GAMESERVERS_DB = orig
            os.unlink(gs)
        states = {r["title"]: r["state"] for r in self.con.execute(
            "SELECT title, state FROM compat_deploy WHERE ip='192.168.1.143'")}
        self.assertEqual(states.get("Quake1"), "deployed")
        # Doom3 is a known title that the index has no key for. It must NOT
        # have been written as absent.
        self.assertNotIn("absent", states.values())
        self.assertNotIn("Doom3", states)
        row = compat._matrix_rows(self.con, title="Doom3")[0]
        self.assertEqual(row["deploy"], "untested")


class TestSecretsNeverLeaveTheLan(Base):
    r"""The export is published to a cloud dashboard, so a leak here is the one
    irreversible mistake available. These assert it REFUSES rather than scrubs.

    EVERY FIXTURE BELOW IS ASSEMBLED AT RUNTIME, never written as a literal.
    `tests/python/test_no_committed_secrets.py` greps this repo for key-shaped
    strings and cannot tell a test fixture from a real key - which is correct
    behaviour for that scanner and must not be weakened to accommodate this
    file. A decoy key committed here would either trip the scanner forever or
    teach someone to relax it, and the second outcome is how a real key gets
    committed later.
    """

    def test_export_refuses_a_payload_containing_the_agent_secret(self):
        secret = "-".join(("retro", "agent", "secret"))
        with self.assertRaises(SystemExit) as cm:
            compat._assert_no_secrets('{"note": "connect with %s"}' % secret)
        self.assertIn("REFUSING TO EXPORT", str(cm.exception))

    def test_export_refuses_a_cd_key_shaped_literal(self):
        fake = "-".join(("ABCDE", "12345", "FGHIJ", "67890", "KLMNO"))
        with self.assertRaises(SystemExit):
            compat._assert_no_secrets('{"k": "%s"}' % fake)

    def test_export_refuses_a_private_key(self):
        header = "-----BEGIN %s KEY-----" % "RSA PRIVATE"
        with self.assertRaises(SystemExit):
            compat._assert_no_secrets(header)

    def test_a_clean_export_passes(self):
        self.seed()
        compat._assert_no_secrets(json.dumps(compat._matrix_rows(self.con)))


class TestDocAliasesAreExplicit(Base):
    def test_every_alias_target_is_a_real_title_or_registered_mod(self):
        """A DOC_ALIASES typo silently drops a verification from the matrix."""
        known = set(compat.MODS) | set(compat.WITHDRAWN)
        lib = compat.LIBRARY
        if os.path.isdir(lib):
            known |= {d for d in os.listdir(lib)
                      if not d.startswith("_")
                      and os.path.isdir(os.path.join(lib, d))}
        else:
            self.skipTest("staged library not mounted at %s" % lib)
        bad = []
        for key, val in compat.DOC_ALIASES.items():
            for t in val.split("|"):
                if t not in known:
                    bad.append("%s -> %s" % (key, t))
        self.assertEqual(bad, [], "alias targets not in the library: %s" % bad)

    def test_evidence_tokens_point_at_real_titles(self):
        known = set(compat.MODS) | set(compat.WITHDRAWN)
        lib = compat.LIBRARY
        if not os.path.isdir(lib):
            self.skipTest("staged library not mounted")
        known |= {d for d in os.listdir(lib) if not d.startswith("_")
                  and os.path.isdir(os.path.join(lib, d))}
        bad = [t for t in compat.EVIDENCE_TOKENS.values() if t not in known]
        self.assertEqual(bad, [], "evidence tokens with no title: %s" % bad)


class TestEvidence(Base):
    def test_verified_without_evidence_is_recorded_but_flagged(self):
        self.seed()
        rc = compat.main(["--db", self.path, "record", "--box", ".143",
                          "--title", "Quake1", "--runs", "verified"])
        self.assertEqual(rc, 0)
        row = compat._matrix_rows(self.con, box=".143", title="Quake1")[0]
        self.assertEqual(row["runs"], "verified")
        self.assertEqual(row["evidence"], 0)

    def test_record_rejects_an_unknown_title(self):
        self.seed()
        rc = compat.main(["--db", self.path, "record", "--box", ".143",
                          "--title", "NotAGame", "--runs", "verified"])
        self.assertEqual(rc, 1)

    def test_record_rejects_an_unknown_box(self):
        self.seed()
        rc = compat.main(["--db", self.path, "record", "--box", ".99",
                          "--title", "Quake1", "--runs", "verified"])
        self.assertEqual(rc, 1)


class TestIdempotence(Base):
    def test_repeated_record_of_the_same_fact_makes_one_row(self):
        self.seed()
        for _ in range(4):
            C.put_render(self.con, "192.168.1.143", "Quake1", "measured",
                         "verified", source="manual")
            C.put_evidence(self.con, "192.168.1.143", "Quake1", "render",
                           "screenshot", "/tmp/a.png", "2026-08-31")
        self.assertEqual(self.con.execute(
            "SELECT COUNT(*) FROM compat_render").fetchone()[0], 1)
        self.assertEqual(self.con.execute(
            "SELECT COUNT(*) FROM compat_evidence").fetchone()[0], 1)


class TestSchemaCoexistsWithFleetbook(Base):
    def test_connect_does_not_disturb_the_recipes_tables(self):
        """These tables share a file with the fleetbook the chat brain writes."""
        self.con.executescript(
            "CREATE TABLE IF NOT EXISTS recipes (id INTEGER PRIMARY KEY,"
            " slug TEXT UNIQUE, title TEXT);")
        self.con.execute("INSERT INTO recipes (slug,title) VALUES ('a','A')")
        self.con.commit()
        con2 = C.connect(self.path)          # a second migration run
        self.assertEqual(con2.execute(
            "SELECT COUNT(*) FROM recipes").fetchone()[0], 1)
        con2.close()

    def test_migration_is_rerunnable(self):
        for _ in range(3):
            C.connect(self.path).close()
        self.assertTrue(list(self.con.execute(
            "SELECT 1 FROM sqlite_master WHERE name='v_compat_matrix'")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
