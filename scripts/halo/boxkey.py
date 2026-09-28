"""The box-local copy of a box's own Halo key - shared by assign_keys.py and
audit_keys.py.

WHY (measured 2026-09-28): Halo allows one simultaneous player per key, so
assign_keys.py gives each box its own DigitalProductID. But GAMESYNC merges a
title's install.reg every time it walks the title, and Halo's carries the
library's single key - .145 and .240, given distinct keys on 2026-09-01, were
found back on the SAME key, unable to be in one game. So assign_keys.py also
keeps the box's key here, OUTSIDE the game tree (a purge of C:\\Games\\Halo must
not take it), and the staged `Play Halo.bat` re-applies it on every launch
(scripts/fleet/stage-fleetres.py, halo_boxkey()). The registry value can
therefore be the library key between a sync and the next launch; the key a
game actually runs with is THIS file's, which is what the audit compares.

Nothing here prints a key: fingerprints only.
"""
import hashlib
import re

# Under %ALLUSERSPROFILE%, which is per box (D:\ on .124's XP), so it is
# resolved ON the box. The launcher's spelling is stage-fleetres.py's
# HALO_BOXKEY_BAT; tests/python/test_halo_boxkey.py pins the two together.
BOXKEY_REL = "RetroFleet\\halo-key.reg"


def dpid_fp(hexs):
    """The audit's fingerprint of a DigitalProductID given as hex digits -
    identical to how audit_keys.py hashes the value it reads from the
    registry, so the two can be compared."""
    hexs = re.sub(r"[^0-9a-fA-F]", "", hexs).lower()
    return hashlib.sha256(hexs.encode()).hexdigest()[:10] if hexs else None


def dpid_fp_from_reg(text):
    """Fingerprint of the "DigitalProductID"=hex:... value in a .reg file, or
    None. Continuation lines (a trailing backslash) belong to the value."""
    m = re.search(r'"DigitalProductID"\s*=\s*hex:((?:[^\r\n]*\\\r?\n)*[^\r\n]*)',
                  text, re.I)
    if not m:
        return None
    return dpid_fp(m.group(1).replace("\\", ""))


async def boxkey_path(conn):
    """Absolute path of the box-local key file, resolved on the box."""
    out = (await conn.command_text("EXEC cmd /c echo %ALLUSERSPROFILE%",
                                   timeout=20.0)).strip()
    last = out.splitlines()[-1].strip() if out else ""
    if not last or "%" in last or ":" not in last:
        raise RuntimeError("cannot resolve %%ALLUSERSPROFILE%% on the box (got %r)"
                           % last[:60])
    return last + "\\" + BOXKEY_REL
