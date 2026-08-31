#!/usr/bin/env python3
"""The two Wine-hosted game servers: Descent 3 and Far Cry.

Neither game ever shipped a Linux dedicated server, so both run on the dev host
as the ORIGINAL Windows binary under Wine inside a container (the same reason
Tribes 2 is a container - it needs a userland this host does not have).

These tests pin the two things that were actually wrong while building them,
because both failed in this project's signature way - reporting success:

  1. `wine <game>.exe` RETURNS AS SOON AS WINESERVER OWNS THE PROCESS.  A unit
     whose ExecStart was just `xvfb-run wine main.exe` therefore "succeeded" in
     a second, xvfb-run tore the X server down, the game died with it, and
     systemd still read `active (running)` because the docker client was alive.
     The launcher must block on `wineserver -w`.

  2. Far Cry's console is a Win32 edit control.  Typed at with
     `xdotool type --window <id>` it receives NO MODIFIER STATE: the console
     echoed `start-server mp-monkeybay` and `g-gametype ffa` - every underscore
     a hyphen, every capital lower case - and answered "Unknown command", while
     the container went on looking healthy.  The typing must go through XTEST
     (no --window) so real Shift-downs are sent.

Neither of those is checkable from Python at runtime, so what is pinned here is
the SOURCE of the launch scripts plus the gameservers.py wiring.
"""
import os
import re
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "scripts", "game-servers"))

HOME = os.path.expanduser("~")
D3_ENTRY = os.path.join(HOME, "descent3-server", "_run", "entry.sh")
FC_ENTRY = os.path.join(HOME, "farcry-server", "_run", "fc-entry.sh")


class TestServerTable(unittest.TestCase):
    """The fleet's server enumerator has to know about them, or the status wall
    silently drops two running servers off the board."""

    def setUp(self):
        import gameservers
        self.gs = gameservers
        self.by_unit = {s["unit"]: s for s in gameservers.SERVERS}

    def test_both_servers_are_listed(self):
        self.assertIn("descent3-server", self.by_unit)
        self.assertIn("farcry-server", self.by_unit)

    def test_descent3_probes_tcp_2092(self):
        row = self.by_unit["descent3-server"]
        self.assertEqual(row["probe"], "d3")
        self.assertEqual(row["port"], 2092)
        self.assertEqual(row["join"], 2092)

    def test_farcry_probes_the_bound_port_not_a_made_up_query(self):
        """Far Cry answers no query we can spell.  The probe must be the honest
        `is UDP 49001 bound` check, NOT a fabricated `\\status\\` that could only
        ever report the server down."""
        row = self.by_unit["farcry-server"]
        self.assertEqual(row["probe"], "udp_bound")
        self.assertEqual(row["port"], 49001)

    def test_every_probe_name_resolves(self):
        for spec in self.gs.SERVERS:
            self.assertIn(spec["probe"], self.gs.PROBES,
                          f"{spec['unit']} names a probe that does not exist")

    def test_udp_bound_refuses_a_remote_host(self):
        """It can only see THIS host's sockets.  Asked about another machine it
        must say "no answer", never guess - a probe that reports a remote server
        up from a local socket is worse than no probe."""
        self.assertIsNone(self.gs.probe_udp_bound(49001, host="10.99.99.99"))


class TestLaunchScripts(unittest.TestCase):
    """Skips loudly when the server trees are not installed on this machine."""

    def _read(self, path):
        if not os.path.exists(path):
            self.skipTest(f"{path} not present - Wine server not installed here")
        return open(path).read()

    def test_descent3_entry_blocks_on_wineserver(self):
        body = self._read(D3_ENTRY)
        self.assertRegex(body, r"wine main\.exe .*&\s*$|wine main\.exe .*&\n",
                         "main.exe must be backgrounded")
        self.assertIn("wineserver -w", body,
                      "without `wineserver -w` the script exits at once and "
                      "xvfb-run kills the server it just started")

    def test_farcry_entry_types_through_xtest(self):
        body = self._read(FC_ENTRY)
        self.assertIn("wineserver -w", body)
        self.assertIn("start_server", body)
        # The bug: `xdotool type --window $W` drops Shift, so start_server
        # arrives as start-server.  Any `type --window` here is that bug.
        for line in body.splitlines():
            stripped = line.strip()
            if stripped.startswith("#") or stripped.startswith("rem"):
                continue
            if "xdotool type" in stripped:
                self.assertNotIn("--window", stripped,
                                 "xdotool type --window loses modifier state "
                                 "under Wine: start_server becomes start-server")

    def test_farcry_entry_says_so_when_the_console_never_appears(self):
        """A container that stays up is not a server that is hosting."""
        body = self._read(FC_ENTRY)
        self.assertIn("FAILED", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
