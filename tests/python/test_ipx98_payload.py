"""The Windows 98 SE IPX/SPX payload IPXSETUP installs (agent 1.97.0).

agent/src/ipx9x.c copies NWLINK.VXD and WSIPX.VXD from
\\\\192.168.1.122\\files\\Utility\\Retro Automation\\ipx\\win98se\\ into a Win98
box's SYSTEM folder - and only if each matches the size and CRC-32 compiled into
it (agent/shared/ipxplan.h ipx9x_payload[]). scripts/fleet/stage-ipx98-payload.py
puts them there from the Windows 98 SE CD. The two must agree on WHERE and on
WHAT, or the agent refuses every install with "not the expected build":

* the stager reads its manifest FROM ipxplan.h - one source of truth;
* the agent's default folder is the stager's destination;
* the destination is never inside Games-Library (a new directory there makes
  retro-autodeploy resync the whole fleet);
* MANIFEST.TXT is written LAST (the agent treats the folder as reachable only
  once it exists) and is deterministic CRLF text;
* share-side, skipping LOUDLY when the share is not mounted: what is published
  is exactly the manifest's build.
"""
import importlib.util
import os
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
STAGER = ROOT / "scripts" / "fleet" / "stage-ipx98-payload.py"
HDR = (ROOT / "agent" / "shared" / "ipxplan.h").read_text()


def stager():
    spec = importlib.util.spec_from_file_location("stage_ipx98", STAGER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_the_stager_takes_its_manifest_from_the_agent():
    s = stager()
    files = s.manifest_from_header()
    assert [f[0] for f in files] == ["NWLINK.VXD", "WSIPX.VXD"]
    assert files[0][1:] == (51010, 0x5979320A, "d92a994f76be8ce6469affa6f3530300")
    assert files[1][1:] == (14526, 0x27ACFFF1, "ec91ebf166e66bf7108dd93d374c630d")
    assert set(s.CAB_OF) == {f[0] for f in files}, "every payload file has its cabinet"


def test_the_agent_looks_where_the_stager_writes():
    s = stager()
    m = re.search(r'#define IPX_DEFAULT_PAYLOAD "([^"]+)"', HDR)
    assert m
    agent = m.group(1).replace("\\\\", "\\")
    assert agent.lower().endswith("\\" + s.DEST.replace("/", "\\").lower()), (agent, s.DEST)
    assert agent.lower().startswith("\\\\192.168.1.122\\files\\")
    assert '#define IPX_PAYLOAD_MANIFEST "MANIFEST.TXT"' in HDR and s.MANIFEST == "MANIFEST.TXT"


def test_never_into_the_game_library():
    s = stager()
    s.check_dest(s.DEST)
    for bad in ("Files/Games-Library/ipx", "Files\\Games-Library\\x", "files/games-library"):
        with pytest.raises(SystemExit):
            s.check_dest(bad)


def test_the_manifest_text_is_crlf_and_names_every_file():
    s = stager()
    files = s.manifest_from_header()
    t = s.manifest_text(files)
    assert t == s.manifest_text(files), "deterministic - a re-run must not rewrite it"
    assert b"\r\n" in t and b"\n" not in t.replace(b"\r\n", b"")
    for n, size, crc, md5 in files:
        assert ("%s %d %08X %s" % (n, size, crc, md5)).encode() in t


def test_manifest_last_and_one_verified_put_per_file():
    src = STAGER.read_text()
    main = src[src.index("def main("):]
    assert main.index('sw.put(got[n], "%s/%s" % (DEST, n), auth)') < \
        main.index('sw.put(mlocal, "%s/%s" % (DEST, MANIFEST), auth)')
    assert "PUBLISH FAILED" in main and "return 1" in main
    # nothing reaches the share except through sharewrite's verified put
    assert "shutil.copy" not in src and "/run/user" not in src


def test_the_published_payload_is_the_agents_build():
    s = stager()
    if not os.path.isdir(os.path.join(s.MNT, "Utility")):
        pytest.skip("SKIPPED LOUDLY: %s is not mounted - the published IPX payload was NOT "
                    "checked" % s.MNT)
    state = s.share_state(s.manifest_from_header())
    bad = {n: w for n, w in state.items() if w is not None}
    assert not bad, ("the share's IPX payload is not current - run "
                     "python3 scripts/fleet/stage-ipx98-payload.py: %s" % bad)
