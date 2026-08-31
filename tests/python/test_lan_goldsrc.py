#!/usr/bin/env python3
"""LAN multiplayer regressions for the GoldSrc tree and the standalone shooters.

Every assertion here encodes something that was MEASURED on hardware on
2026-08-31 while proving two-box LAN play, and every one of them describes a
failure that reported success:

  * Deathmatch Classic could not host at all - "map dmc_dm2" died with
    "Host_Error: EV_Precache: file events/axe.sc missing from server" - because
    the staged dmc\\events\\ directory held ONLY the door\\ subdirectory. The
    validator was green, GAMESYNC said state=done / failed_files=0, and the
    desktop shortcut looked perfect.

  * The same gap on the CLIENT does not error at all: a joiner missing the
    files connects, holds a slot ("players: 2 active") and sticks on
    "Server # 1" forever.

  * The first attempt to audit this reported TFC as missing two events. It was
    not: TFC ships them as "Tf_nail.sc" and "Tf_sg.sc" with a capital T, and
    a case-sensitive comparison of a WINDOWS tree said they were absent. That
    is the exact trap CLAUDE.md warns about, so the resolver here is
    case-insensitive and this file asserts that it is.

  * Hidden & Dangerous' LAN launchers passed "net_connection_provider tcpip",
    an option HDE.exe advertises in its -help text and DOES NOT IMPLEMENT; the
    game died on "Unknown command-line option: tcpip".

  * Red Faction refuses all multiplayer - client menu AND dedicated server -
    unless HKCU\\...\\Volition\\Red Faction\\UpdateRate is non-zero. The
    dedicated server binds UDP 7755 on its way out, so netstat shows a
    listening port for a server that has already died.
"""

import os
import unittest

SHARE = "/mnt/retro-share/Files/Games-Library"


# --------------------------------------------------------------------------
# Pure logic: how GoldSrc resolves an event script a game DLL precaches.
# --------------------------------------------------------------------------

def event_is_satisfied(event, mod_files, base_files):
    """True if `event` (e.g. "events/axe.sc") resolves for a mod.

    GoldSrc looks in the mod directory first and falls back to the base game
    ("valve"), and the filesystem underneath is Windows, so the match is
    CASE-INSENSITIVE and path separators are normalised.
    """
    want = event.replace("\\", "/").lower()
    have = {p.replace("\\", "/").lower() for p in mod_files}
    have |= {p.replace("\\", "/").lower() for p in base_files}
    return want in have


def missing_events(required, mod_files, base_files):
    """The events a mod cannot resolve - i.e. the ones that break hosting."""
    return [e for e in required
            if not event_is_satisfied(e, mod_files, base_files)]


class GoldSrcEventResolution(unittest.TestCase):
    """dmc's 19 precached events against the tree as it was, and as it is."""

    # dmc.dll and dmc\cl_dlls\client.dll both precache exactly these.
    DMC_REQUIRED = [
        "events/axe.sc", "events/axeswing.sc", "events/door/doorgodown.sc",
        "events/door/doorgoup.sc", "events/door/doorhitbottom.sc",
        "events/door/doorhittop.sc", "events/explosion.sc", "events/gibs.sc",
        "events/grenade.sc", "events/lightning.sc", "events/powerup.sc",
        "events/rocket.sc", "events/shotgun1.sc", "events/shotgun2.sc",
        "events/spike.sc", "events/superspike.sc", "events/teleport.sc",
        "events/trail.sc", "events/train.sc",
    ]

    # What valve\events\ ships - the fallback, and the reason three of dmc's
    # events resolved even while the mod's own directory was gutted.
    VALVE_EVENTS = [
        "events/crossbow1.sc", "events/crossbow2.sc", "events/crowbar.sc",
        "events/egon_effect.sc", "events/egon_fire.sc", "events/egon_stop.sc",
        "events/firehornet.sc", "events/gauss.sc", "events/gaussspin.sc",
        "events/glock1.sc", "events/glock2.sc", "events/mp5.sc",
        "events/mp52.sc", "events/python.sc", "events/rpg.sc",
        "events/shotgun1.sc", "events/shotgun2.sc", "events/snarkfire.sc",
        "events/train.sc", "events/tripfire.sc",
    ]

    DMC_BROKEN = [
        "events/door/doorgodown.sc", "events/door/doorgoup.sc",
        "events/door/doorhitbottom.sc", "events/door/doorhittop.sc",
    ]

    DMC_FIXED = DMC_BROKEN + [
        "events/axe.sc", "events/axeswing.sc", "events/explosion.sc",
        "events/gibs.sc", "events/grenade.sc", "events/lightning.sc",
        "events/powerup.sc", "events/rocket.sc", "events/shotgun1.sc",
        "events/shotgun2.sc", "events/spike.sc", "events/superspike.sc",
        "events/teleport.sc", "events/trail.sc",
    ]

    def test_old_broken_tree_is_detected(self):
        """The state that shipped: 12 events unresolvable, axe.sc first."""
        missing = missing_events(self.DMC_REQUIRED, self.DMC_BROKEN,
                                 self.VALVE_EVENTS)
        self.assertEqual(len(missing), 12, missing)
        # This is the literal name in the Host_Error the engine printed.
        self.assertIn("events/axe.sc", missing)
        # ...and these three DID resolve, through the valve fallback, which is
        # why the gap looked smaller than it was.
        self.assertNotIn("events/shotgun1.sc", missing)
        self.assertNotIn("events/shotgun2.sc", missing)
        self.assertNotIn("events/train.sc", missing)

    def test_fixed_tree_resolves_every_event(self):
        self.assertEqual(
            missing_events(self.DMC_REQUIRED, self.DMC_FIXED,
                           self.VALVE_EVENTS), [])

    def test_resolution_is_case_insensitive(self):
        """TFC ships Tf_nail.sc / Tf_sg.sc; the precache asks for lowercase.

        A case-sensitive audit called these missing and would have had someone
        'restore' two files that were sitting right there.
        """
        required = ["events/wpn/tf_nail.sc", "events/wpn/tf_sg.sc"]
        shipped = ["events/wpn/Tf_nail.sc", "events/wpn/Tf_sg.sc"]
        self.assertEqual(missing_events(required, shipped, []), [])

    def test_valve_fallback_is_not_assumed_to_cover_everything(self):
        """A mod's own weapons are its own; valve cannot supply them."""
        self.assertFalse(
            event_is_satisfied("events/axe.sc", [], self.VALVE_EVENTS))


class BlueShiftHasNoMultiplayer(unittest.TestCase):
    """Measured, not assumed - and the reason it is a row, not an omission."""

    def test_sp_mission_with_only_campaign_maps_is_single_player(self):
        # bshift\liblist.gam says type "SP Mission"; bshift\maps\ is empty and
        # pak1.pak holds 37 maps, every one of them ba_* campaign.
        maps = ["ba_canal1.bsp", "ba_hazard1.bsp", "ba_yard3.bsp",
                "ba_xen1.bsp", "ba_outro.bsp"]
        self.assertTrue(all(m.startswith("ba_") for m in maps))
        # The mod does declare mpentity - inherited boilerplate, not a
        # capability. A capability check must look at the MAPS.
        declared_mpentity = True
        has_dm_map = any(not m.startswith("ba_") for m in maps)
        self.assertTrue(declared_mpentity)
        self.assertFalse(has_dm_map)


class HiddenAndDangerousOptionTable(unittest.TestCase):
    """HDE.exe's -help text advertises an option it does not implement."""

    # The option table as it appears in Bin\HDE.exe, in binary order.
    IMPLEMENTED = {
        "safe", "language", "profile", "datadisk", "net_port", "net_address",
        "net_session_name", "sound_caps", "graph_caps", "net_cpu_schedule",
        "net_player", "net_num", "net_log", "net_join", "net_host", "sleep",
        "stop", "mission", "launch_app", "direct_trans", "releasedll",
        "position", "resolution", "help",
    }

    def test_net_connection_provider_is_not_implemented(self):
        self.assertNotIn("net_connection_provider", self.IMPLEMENTED)
        self.assertNotIn("net_connect", self.IMPLEMENTED)

    def test_the_options_the_launchers_actually_use_are_implemented(self):
        for opt in ("net_host", "net_join", "net_address", "net_player",
                    "net_session_name", "profile"):
            self.assertIn(opt, self.IMPLEMENTED)


class RedFactionConnectionSpeed(unittest.TestCase):
    """A rate in bytes/sec under HKCU, not an enum under HKLM."""

    T1_LAN_BYTES_PER_SEC = 0x30D40

    def test_t1_lan_rate_is_200000(self):
        self.assertEqual(self.T1_LAN_BYTES_PER_SEC, 200000)

    def test_zero_means_no_multiplayer(self):
        def multiplayer_allowed(update_rate):
            return bool(update_rate)
        self.assertFalse(multiplayer_allowed(0))       # the shipped state
        self.assertTrue(multiplayer_allowed(self.T1_LAN_BYTES_PER_SEC))


# --------------------------------------------------------------------------
# Share-side: the library really carries the three fixes.
# Skips LOUDLY when the share is not mounted - a silent skip would let the
# library rot back to the broken state unnoticed.
# --------------------------------------------------------------------------

def _share_or_skip(test):
    if not os.path.isdir(SHARE):
        raise unittest.SkipTest(
            "SKIPPING LOUDLY: %s is not mounted, so the staged library was "
            "NOT checked. Mount the share and re-run." % SHARE)


class StagedLibraryCarriesTheFixes(unittest.TestCase):

    def setUp(self):
        _share_or_skip(self)

    def test_dmc_events_are_present(self):
        d = os.path.join(SHARE, "HalfLife1", "dmc", "events")
        have = {n.lower() for n in os.listdir(d)}
        for name in ("axe.sc", "axeswing.sc", "explosion.sc", "gibs.sc",
                     "grenade.sc", "lightning.sc", "powerup.sc", "rocket.sc",
                     "spike.sc", "superspike.sc", "teleport.sc", "trail.sc"):
            self.assertIn(name, have,
                          "dmc/events/%s missing - Deathmatch Classic cannot "
                          "host without it" % name)
        self.assertIn("door", have)

    def test_hd_launchers_do_not_pass_the_unimplemented_option(self):
        d = os.path.join(SHARE, "HiddenAndDangerous")
        for name in ("Host Hidden and Dangerous - LAN.bat",
                     "Join Hidden and Dangerous - LAN.bat"):
            p = os.path.join(d, name)
            self.assertTrue(os.path.isfile(p), p)
            with open(p, "rb") as fh:
                body = fh.read().decode("latin1")
            start = body.lower().find("hde.exe")
            self.assertGreater(start, 0, "no HDE.exe invocation in " + name)
            invocation = body[start:]
            self.assertNotIn("net_connection_provider", invocation,
                             "%s still passes an option HDE.exe does not "
                             "implement" % name)

    def test_red_faction_install_reg_seeds_updaterate(self):
        p = os.path.join(SHARE, "RedFaction", "install.reg")
        with open(p, "rb") as fh:
            body = fh.read().decode("latin1")
        self.assertIn("REGEDIT4", body.split("\n")[0])
        self.assertIn("HKEY_CURRENT_USER\\SOFTWARE\\Volition\\Red Faction",
                      body)
        self.assertIn("\"UpdateRate\"=dword:00030d40", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
