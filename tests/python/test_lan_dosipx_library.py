#!/usr/bin/env python3
"""Staged-library invariants for the DOS/IPX and peer-hosted LAN titles.

SKIPS LOUDLY when the SMB share is not mounted - a silent skip would let the
library rot unnoticed, which is the same reasoning as test_staged_library.py.

Each assertion below is a fix that was measured on hardware on 2026-08-31 and
would otherwise regress invisibly.
"""
import os
import unittest

LIB = "/mnt/retro-share/Files/Games-Library"

DOSBOX_JOINERS = [
    ("Descent1", "Join Descent - LAN.bat"),
    ("Carmageddon1", "Join Carmageddon - LAN.bat"),
    ("RedneckRampage", "Join Redneck Rampage - LAN.bat"),
]


def _read(*parts):
    return open(os.path.join(LIB, *parts), "rb").read().decode("latin-1")


class LibraryCase(unittest.TestCase):
    def setUp(self):
        if not os.path.isdir(LIB):
            self.skipTest(f"SKIPPED LOUDLY: {LIB} is not mounted - the staged "
                          "library could not be checked at all")


class TestDosboxJoinRetries(LibraryCase):
    """The joiner races the host's DOSBox startup.

    DOSBox's IPX client gives up after a FIVE SECOND timeout and then runs the
    game anyway, so a joiner that started first lands in the netgame browser
    with no tunnel and nothing on screen says why - the "Timeout connecting to
    server" line is in the DOSBox status window, behind the fullscreen game.
    Measured .123 -> .124: both launched together, the P3 host was slower to
    reach IPXNET STARTSERVER, and the joiner timed out while UDP 213 on the
    host was listening seconds later.
    """

    def test_every_joiner_retries(self):
        for title, bat in DOSBOX_JOINERS:
            body = _read(title, bat)
            n = body.count("echo IPXNET CONNECT %HOSTIP%")
            self.assertGreaterEqual(
                n, 3,
                f"{title}/{bat} writes only {n} IPXNET CONNECT line(s); one "
                "attempt loses the race against a slow host")

    def test_no_joiner_prompts(self):
        """A bare `set /p` hangs for ever on a box with nobody at the keyboard,
        which looks exactly like a game that failed to start."""
        for title, bat in DOSBOX_JOINERS:
            body = _read(title, bat)
            self.assertIn("C:\\Games\\lanhost.txt", body,
                          f"{title}/{bat} must fall back to the fleet-wide "
                          "lanhost.txt before ever prompting")


class TestCarmageddon1LanConf(LibraryCase):
    """Carmageddon's main menu has an ATTRACT-MODE TIMEOUT and its opening FMV
    runs about two minutes.  The player who reaches the network menu first is
    dropped back into the intro before the second one arrives, and ESC from
    attract mode restarts the opening rather than returning to the menu.
    -nocutscenes (a real MAINPROG.EXE switch, read out of the binary) removes
    both problems for the LAN path.  Single player deliberately keeps its
    cutscenes - that is dosboxCarma_single.conf and is untouched.
    """

    def test_lan_conf_exists_and_skips_cutscenes(self):
        body = _read("Carmageddon1", "dosboxCarma_lan.conf")
        self.assertIn("-nocutscenes", body)
        self.assertIn("MAINPROG.EXE", body)

    def test_single_player_conf_still_has_its_cutscenes(self):
        body = _read("Carmageddon1", "dosboxCarma_single.conf")
        self.assertNotIn("-nocutscenes", body)

    def test_both_lan_launchers_use_it(self):
        for bat in ("Host Carmageddon - LAN.bat", "Join Carmageddon - LAN.bat"):
            body = _read("Carmageddon1", bat)
            self.assertIn("dosboxCarma_lan.conf", body)
            self.assertNotIn("dosboxCarma_single.conf", body)


class TestDescent3Lan(LibraryCase):
    """Descent 3 has a real dedicated server (main.exe -dedicated) and a real
    command-line join.  The join needs THREE switches, and -pilot is the one
    that was missing: without it the game stops on the PILOTS modal before
    +connect is ever reached, which is why an earlier note recorded "+connect
    only reaches the main menu".
    """

    def test_launch_txt_offers_host_and_join(self):
        body = _read("Descent3", "launch.txt")
        data = [l for l in body.splitlines()
                if l.strip() and not l.lstrip().startswith("#")]
        targets = [l.split("\t")[0] for l in data]
        self.assertIn("Host Descent 3 - LAN.bat", targets)
        self.assertIn("Join Descent 3 - LAN.bat", targets)
        # the agent only reads the first 1023 bytes
        self.assertLessEqual(len("\r\n".join(data).encode()), 1023)

    def test_launchers_exist_and_have_no_parentheses(self):
        for name in ("Host Descent 3 - LAN.bat", "Join Descent 3 - LAN.bat"):
            self.assertTrue(os.path.exists(os.path.join(LIB, "Descent3", name)),
                            f"{name} is named in launch.txt but not in the tree")
            self.assertNotIn("(", name)
            self.assertNotIn(")", name)

    def test_host_runs_the_dedicated_server(self):
        body = _read("Descent3", "Host Descent 3 - LAN.bat")
        self.assertIn("-dedicated", body)
        self.assertIn("Dedicated.cfg", body)

    def test_join_carries_pilot_directip_and_connect(self):
        body = _read("Descent3", "Join Descent 3 - LAN.bat")
        for switch in ("-pilot", "-directip", "+connect"):
            self.assertIn(switch, body,
                          f"{switch} missing - without -pilot the PILOTS modal "
                          "blocks the join and +connect looks ignored")

    def test_join_never_prompts(self):
        body = _read("Descent3", "Join Descent 3 - LAN.bat")
        # Comments talk ABOUT set /p on purpose - only real code counts.
        code = [l for l in body.splitlines()
                if not l.strip().lower().startswith("rem")]
        self.assertNotIn("set /p", "\n".join(code))
        self.assertIn("192.168.1.132", body,
                      "the dev host's Wine server is the last-resort default")


if __name__ == "__main__":
    unittest.main(verbosity=2)
