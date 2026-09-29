r"""The favourites agent's 2026-09-29 fixes (scripts/gameindex), one section each.

Everything here runs on the dev host with no box: a fake agent (FakeBox) holds
an in-memory filesystem, a process list and a registry answer, and the real
push_favorites() runs against it. Share-dependent checks read the staged
library through /mnt/retro-share and SKIP LOUDLY when it is not mounted.

  1. Labels.  A public server's hostname went into a Quake `//` comment with
     only newlines stripped. Quake II/III split a config line at `;` outside
     quotes EVEN INSIDE A COMMENT, so `x; quit` would have run `quit` on every
     fleet box at every start. Proven here against a transcription of
     ioq3 1.36's Cbuf_Execute.
  2. Team Arena.  Its Favorites tab reads the ENGINE's list, saved in
     %APPDATA%\Quake3\servercache.dat by ioquake3 1.36 - never server1..16.
     The layout is pinned to the staged ioquake3.x86.exe itself.
  3. BUSY per FILE.  quake3 and ioquake3 share baseq3\autoexec.cfg; with
     ioquake3 running the old loop said "ioquake3 BUSY" and then let "quake3"
     write the same file. And Win98's upper-case PROCLIST never matched.
  4. A settled box says "unchanged".  Same servers in another order, other
     labels, CS re-saving its vdf with its own tabs, UT99 seeds sharing a host,
     and a ranking that reshuffled the cut every pass all rewrote files while
     nothing a player could see had changed.
  5. Skip reasons that were no longer true, and two writers (RTCW, Deus Ex)
     that reported success for files no game reads.

Run: python3 -m pytest -q tests/python/test_patch_favourites.py
"""
import asyncio
import hashlib
import json
import os
import pathlib
import re
import socket
import struct
import sys
import threading
import time

import pytest

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "scripts" / "gameindex"))
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import db          # noqa: E402
import favorites   # noqa: E402
import masters     # noqa: E402
import sync        # noqa: E402

LIBRARY = pathlib.Path("/mnt/retro-share/Files/Games-Library")
ME = "192.168.1.132"


def need_share():
    if not LIBRARY.is_dir():
        pytest.skip("SKIPPED LOUDLY: /mnt/retro-share/Files/Games-Library is not "
                    "mounted - this check reads the staged library and did NOT run")


# --- fixtures ----------------------------------------------------------------

@pytest.fixture()
def con(tmp_path):
    c = db.connect(str(tmp_path / "t.db"))
    yield c
    c.close()


def srow(addr, gamename="baseq3", name=None, players=4, local=0,
         source="q3master", ping=30, query_port=0):
    return {"addr": addr, "hostname": name if name is not None else f"srv {addr}",
            "map": "", "players": players, "maxplayers": 16, "ping_ms": ping,
            "gamename": gamename, "passworded": 0, "is_local": local,
            "source": source, "query_port": query_port}


def local_q3(con):
    db.upsert_servers(con, "q3", [
        srow(f"{ME}:27961", "baseq3", "NSC Retro Fleet Arena (Q3A)", 0, 1, "local"),
        srow(f"{ME}:27962", "missionpack", "NSC Retro Fleet Arena (Team Arena)",
             0, 1, "local"),
        srow(f"{ME}:27960", "baseoa", "NSC Retro Fleet Arena (OA)", 0, 1, "local")])


class FakeBox:
    """An in-memory retro agent answering the commands push_favorites sends."""

    def __init__(self, files=None, dirs=(), procs=(), gamesync="idle",
                 appdata=r"C:\Documents and Settings\Administrator\Application Data",
                 greeting="RETRO BOX Win5.1", proclist_fails=False):
        self.files = {}
        self.dirs = set()
        for d in dirs:
            self.mkdirs(d)
        for p, data in (files or {}).items():
            self.put(p, data)
        self.procs = list(procs)
        self.gamesync = gamesync
        self.appdata = appdata
        self.greeting = greeting
        self.proclist_fails = proclist_fails
        self.sent = []

    # filesystem helpers (Windows: case-insensitive, backslashes)
    @staticmethod
    def _k(p):
        return p.replace("/", "\\").rstrip("\\").lower()

    def mkdirs(self, d):
        parts = d.replace("/", "\\").rstrip("\\").split("\\")
        for i in range(1, len(parts) + 1):
            self.dirs.add(self._k("\\".join(parts[:i])))

    def put(self, path, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        self.mkdirs(path.rsplit("\\", 1)[0])
        self.files[self._k(path)] = (path.rsplit("\\", 1)[1], bytes(data))

    def get(self, path):
        f = self.files.get(self._k(path))
        return None if f is None else f[1]

    def uploads(self):
        return [c for c in self.sent if c.startswith("UPLOAD ")]

    # the protocol surface sync.py uses
    async def send_command(self, cmd, binary_payload=None, timeout=60):
        self.sent.append(cmd)
        verb, _, arg = cmd.partition(" ")
        if verb == "DOWNLOAD":
            data = self.get(arg)
            return (1, data) if data is not None else \
                (0xFF, b"Cannot open file: error 2")
        if verb == "UPLOAD":
            if self._k(arg.rsplit("\\", 1)[0]) not in self.dirs:
                return 0xFF, b"Cannot create file: error 3"
            self.put(arg, binary_payload)
            return 0, b"OK"
        if verb == "MKDIR":
            self.mkdirs(arg)
            return 0, b"OK"
        if verb == "DIRLIST":
            d = self._k(arg)
            out = []
            if d in self.dirs:
                for sub in sorted(self.dirs):
                    if sub.rsplit("\\", 1)[0] == d and sub != d:
                        out.append({"name": sub.rsplit("\\", 1)[1], "is_dir": True})
                for k, (name, data) in sorted(self.files.items()):
                    if k.rsplit("\\", 1)[0] == d:
                        out.append({"name": name, "is_dir": False,
                                    "size": len(data)})
            return 0, json.dumps(out).encode()   # [] for a missing dir, like files.c
        if verb == "PROCLIST":
            if self.proclist_fails:
                return 0xFF, b"PROCLIST failed"
            return 0, json.dumps([{"pid": i + 8, "name": n}
                                  for i, n in enumerate(self.procs)]).encode()
        if verb == "GAMESYNC":
            return 0, json.dumps({"state": self.gamesync}).encode()
        if verb == "REGREAD":
            if self.appdata is None:
                return 0xFF, b"Cannot open key: error 2"
            return 0, json.dumps({"root": "HKCU", "path": "x", "value": {
                "name": "AppData", "type": "REG_SZ", "data": self.appdata}}).encode()
        if verb == "HWPROFILE":
            return 0, b"{}"
        return 0xFF, b"Unknown command"

    async def command_text(self, cmd, timeout=30):
        st, data = await self.send_command(cmd, timeout=timeout)
        if st == 0xFF:
            raise RuntimeError(data.decode("latin-1"))
        return data.decode("latin-1")

    async def command_binary(self, cmd, timeout=60):
        st, data = await self.send_command(cmd, timeout=timeout)
        if st == 0xFF:
            raise RuntimeError(data.decode("latin-1"))
        return data


def push(con, box, monkeypatch, ip="10.9.9.9", dry_run=False):
    async def fake_agent(_ip, fn, timeout=20.0):
        return await fn(box, box.greeting)
    monkeypatch.setattr(sync, "_agent", fake_agent)
    return asyncio.run(sync.push_favorites(con, ip, dry_run=dry_run))


def notes(results):
    return [n for _, _, n in results]


Q3DIR = r"C:\Games\Quake3-TeamArena"
Q3CFG = Q3DIR + r"\baseq3\autoexec.cfg"
STAGED_AUTOEXEC = ('// Fleet autoexec.cfg\nseta r_colorbits "32"\n'
                   'seta com_maxfps "125"\nexec fleetres.cfg\n')


def index_q3(con, ip="10.9.9.9", d=Q3DIR, gamedir_case=None):
    db.replace_games(con, ip, [
        {"key": "quake3", "dir": gamedir_case or d, "engine": "q3",
         "exe": d + r"\quake3.exe"},
        {"key": "ioquake3", "dir": d, "engine": "q3",
         "exe": d + r"\ioquake3.x86.exe"}])
    con.commit()


# =============================================================================
# 1. labels
# =============================================================================

def cbuf_commands(text):
    """Cbuf_Execute's splitter, transcribed from ioq3 1.36 code/qcommon/cmd.c
    (identical in id's 1.32): a command ends at `;` when the double quotes
    seen so far are even, or at `\n`/`\r`. There is no notion of a comment -
    `//` only matters later, to the tokenizer, inside ONE command."""
    out, cur, quotes = [], [], 0
    for ch in text:
        if ch == '"':
            quotes += 1
        if (not quotes & 1 and ch == ";") or ch in "\r\n":
            out.append("".join(cur))
            cur, quotes = [], 0
            continue
        cur.append(ch)
    out.append("".join(cur))
    return [c for c in out if c.strip()]


EVIL = [
    "x; quit",
    'a" ; rcon_password pwned ; "b',
    "line one\nbind MOUSE1 quit",
    "carriage\rvid_restart",
    "nul\x00byte; exec autoexec",
    "tab\tand\x7fdel;disconnect",
]


@pytest.mark.parametrize("engine,key", [("q3", "quake3"), ("q2", "quake2")])
def test_no_hostname_can_add_a_command_to_a_quake_config(engine, key):
    """Every command the engine would run out of our block is ONE favourite
    assignment - whatever the server calls itself."""
    servers = [srow(f"10.0.0.{i + 1}:27960", name=n) for i, n in enumerate(EVIL)]
    text, _ = favorites.render(engine, servers, "", key=key)
    cmds = cbuf_commands(text)
    want = "seta server" if engine == "q3" else "set adr"
    for c in cmds:
        s = c.strip()
        assert s.startswith(want) or s.startswith("//"), \
            f"the engine would execute {s!r} from our file"
    assert sum(1 for c in cmds if c.strip().startswith(want)) == \
        (16 if engine == "q3" else 9)
    assert all(len(line) < 1023 for line in text.splitlines()), \
        "Cbuf copies at most MAX_CMD_LINE-1 bytes; the rest would start a new command"


def test_the_old_writer_really_was_injectable():
    """The shape this guards against, measured the way the engine splits it:
    the pre-fix label rule (strip newlines only) leaves a live `quit`."""
    old_line = 'seta server1 "1.2.3.4:27960"        // ' + \
        "x; quit".replace("\n", " ")[:60]
    assert "quit" in [c.strip() for c in cbuf_commands(old_line)]


def test_clean_label_rules():
    assert favorites.clean_label("a;b", quake=True) == "a b"
    assert favorites.clean_label('say "hi"', quake=True) == "say 'hi'"
    assert favorites.clean_label("x\ny\rz\x00w\x7fv") == "x y z w v"
    assert favorites.clean_label("a;b") == "a;b", "only Quake comments care about ;"
    assert favorites.clean_label("  many    spaces  ") == "many spaces"
    assert len(favorites.clean_label("X" * 400)) == 60


def test_an_unsafe_label_already_on_a_box_is_never_called_unchanged():
    """If a dangerous line is ALREADY in a box's file (an older writer put it
    there), `same_favourites` must not bless it just because the servers
    match - it is rewritten, which is what removes it."""
    servers = [srow("1.2.3.4:27960", name="evil; quit")]
    clean, _ = favorites.render("q3", servers, STAGED_AUTOEXEC, key="quake3")
    assert "// evil quit" in clean, "the ; became a space"
    dirty = clean.replace("// evil quit", "// evil; quit")
    assert "// evil; quit" in dirty
    assert not favorites.same_favourites("q3", dirty, clean)
    assert favorites.same_favourites("q3", clean, clean)


@pytest.mark.parametrize("engine,key", [("unreal", "ut99"), ("ut2k4", "ut2004"),
                                        ("goldsrc", "cs16")])
def test_control_characters_never_reach_any_writer(engine, key):
    r = srow("10.1.1.1:7777", "ut", name="bad\x00name\r\nnext\x1b[31m",
             query_port=7778)
    existing = '"filters"\n{\n\t"favorites"\n\t{\n\t}\n}\n' if engine == "goldsrc" \
        else ("[UBrowser.UBrowserFavoritesFact]\nFavoriteCount=0\n" if engine == "unreal"
              else "[XInterface.ExtendedConsole]\n")
    text, _ = favorites.render(engine, [r], existing, key=key)
    assert not any(ch in text for ch in "\x00\r\x1b"), text


# =============================================================================
# 2. Team Arena - ioquake3 1.36's servercache.dat
# =============================================================================

def test_the_layout_constants():
    assert favorites.SC_REC == 164                 # sizeof(serverInfo_t)
    assert favorites.SC_SIZE == 692736 == 0xA9200   # the header's size field
    assert favorites.SC_LEN == 692748
    assert favorites.SC_GLOBAL * favorites.SC_REC == 0xA4000
    assert favorites.SC_OTHER * favorites.SC_REC == 0x5200
    assert favorites.SC_FAV_OFF == 12 + 0xA4000
    assert favorites.SC_NA_IP == 4
    assert len(favorites.SC_MARK) == 16


def test_the_captured_empty_file_is_what_the_fixtures_build():
    """The servercache.dat downloaded from .123/.145/.195/.240 on 2026-09-28
    (md5 1d9ecd77ad96baa12e160cf4ec596787: counts 0, size 692736, all zero)
    is byte-for-byte the empty file this test builds."""
    assert hashlib.md5(empty_cache()).hexdigest() == \
        "1d9ecd77ad96baa12e160cf4ec596787"


def empty_cache(nglobal=0):
    return struct.pack("<iii", nglobal, 0, favorites.SC_SIZE) + \
        bytes(favorites.SC_SIZE)


def with_favourites(base, recs, nglobal=None):
    b = bytearray(base)
    if nglobal is not None:
        struct.pack_into("<i", b, 0, nglobal)
    struct.pack_into("<i", b, 4, len(recs))
    for i, r in enumerate(recs):
        off = favorites.SC_FAV_OFF + i * favorites.SC_REC
        b[off:off + favorites.SC_REC] = r
    return bytes(b)


def player_added(addr, name):
    """A favourite as the TA UI's addFavorite leaves it: LAN_AddServer with an
    uninitialised netadr_t, so ip6/scope_id hold stack garbage."""
    r = bytearray(favorites.sc_record(addr, name))
    r[8:24] = bytes(range(0xA0, 0xB0))            # not our mark
    struct.pack_into("<I", r, 28, 0xDEADBEEF)
    return bytes(r)


def lan_load_cached_servers(data):
    """ioq3 1.36 LAN_LoadCachedServers + NET_AdrToStringwPort, transcribed.
    Returns the favourites list exactly as the engine would hold it, or [] if
    it would reject the file (size mismatch -> counts reset)."""
    nglobal, nfav, size = struct.unpack_from("<iii", data, 0)
    if size != 692736:
        return []
    out = []
    for i in range(nfav):
        r = data[favorites.SC_FAV_OFF + i * 164:favorites.SC_FAV_OFF + (i + 1) * 164]
        typ = struct.unpack_from("<i", r, 0)[0]
        port = struct.unpack_from(">H", r, 24)[0]          # ntohs(adr.port)
        addr = "%d.%d.%d.%d:%d" % (r[4], r[5], r[6], r[7], port) if typ == 4 \
            else None
        host = r[32:64].split(b"\0", 1)[0].decode("latin-1")
        visible = struct.unpack_from("<i", r, 156)[0]
        out.append((addr, host, visible))
    return out


def net_compare_adr(a, b):
    """NET_CompareAdr for NA_IP, as disassembled from the staged binary: the
    type, the four ip bytes and the port - never ip6."""
    return a[0:4] == b[0:4] and a[4:8] == b[4:8] and a[24:26] == b[24:26]


TA = [srow(f"{ME}:27962", "missionpack", "NSC Retro Fleet Arena (Team Arena)",
           0, 1, "local")]


def test_a_record_is_what_lan_addserver_makes():
    r = favorites.sc_record(f"{ME}:27962", "NSC Retro Fleet Arena (Team Arena)")
    assert len(r) == 164
    assert struct.unpack_from("<i", r, 0)[0] == 4                # NA_IP
    assert r[4:8] == bytes([192, 168, 1, 132])
    assert r[8:24] == favorites.SC_MARK
    assert r[24:26] == bytes([0x6D, 0x3A]), "27962 in NETWORK order (BigShort)"
    assert struct.unpack_from("<I", r, 28)[0] == 0                # scope_id
    assert r[32:64].split(b"\0")[0] == b"NSC Retro Fleet Arena (Team Arena"[:31]
    assert r[63] == 0, "hostName[32] keeps its NUL"
    assert struct.unpack_from("<i", r, 156)[0] == 1               # visible
    assert set(r[64:156]) == {0} and set(r[160:164]) == {0}


def test_a_new_file_loads_in_the_engine_with_the_fleet_server_first():
    new, summary = favorites.sc_merge(None, TA)
    assert len(new) == favorites.SC_LEN
    # hostName[32]: 31 characters and the NUL, as Q_strncpyz leaves it.
    assert lan_load_cached_servers(new) == \
        [(f"{ME}:27962", "NSC Retro Fleet Arena (Team Are", 1)]
    assert summary["ours"] == [f"{ME}:27962"]
    assert struct.unpack_from("<iii", new, 0) == (0, 1, 692736)


def test_the_global_list_and_the_players_favourites_survive_byte_for_byte():
    base = bytearray(empty_cache())
    for i in range(3 * 164):                       # three cached internet servers
        base[12 + i] = (i * 31 + 7) & 0xFF
    theirs = player_added("5.6.7.8:27960", "my own server")
    existing = with_favourites(bytes(base), [theirs], nglobal=3)
    new, summary = favorites.sc_merge(existing, TA)
    assert new[12:favorites.SC_FAV_OFF] == existing[12:favorites.SC_FAV_OFF], \
        "4096 global records untouched"
    assert struct.unpack_from("<i", new, 0)[0] == 3, "numglobalservers kept"
    loaded = lan_load_cached_servers(new)
    assert [a for a, _, _ in loaded] == ["5.6.7.8:27960", f"{ME}:27962"], \
        "the player's entry keeps its place; ours is appended"
    off = favorites.SC_FAV_OFF
    assert new[off:off + 164] == theirs, "their record is not rewritten"
    assert summary["theirs"] == 1


def test_a_second_pass_is_unchanged_even_after_the_game_updates_our_entry():
    first, _ = favorites.sc_merge(None, TA)
    again, _ = favorites.sc_merge(first, TA)
    assert again is None, "the same list twice is 'unchanged'"
    # The game pings the favourite and CL_SetServerInfo rewrites its info.
    b = bytearray(first)
    off = favorites.SC_FAV_OFF
    b[off + 32:off + 64] = b"NSC Retro Fleet Arena ^1(TA)".ljust(32, b"\0")
    b[off + 64:off + 72] = b"mpteam1\0"
    struct.pack_into("<i", b, off + 136, 3)       # clients
    struct.pack_into("<i", b, off + 152, 1)       # ping
    assert favorites.sc_merge(bytes(b), TA)[0] is None, \
        "hostName/map/ping are the game's to update, never a reason to write"


def test_only_our_marked_entries_are_ever_removed():
    stale_ours = favorites.sc_record("9.9.9.9:27960", "gone")
    theirs = player_added("5.6.7.8:27960", "mine")
    ours_now = favorites.sc_record(f"{ME}:27962", "fleet")
    existing = with_favourites(empty_cache(), [stale_ours, theirs, ours_now])
    new, summary = favorites.sc_merge(existing, TA)
    assert [a for a, _, _ in lan_load_cached_servers(new)] == \
        ["5.6.7.8:27960", f"{ME}:27962"]
    assert summary["dropped"] == ["9.9.9.9:27960"]
    # ...and the slot the list no longer uses is zeroed, not left stale.
    off = favorites.SC_FAV_OFF + 2 * 164
    assert set(new[off:off + 164]) == {0}


def test_a_favourite_the_player_already_added_is_not_duplicated():
    theirs = player_added(f"{ME}:27962", "added by hand")
    existing = with_favourites(empty_cache(), [theirs])
    new, summary = favorites.sc_merge(existing, TA)
    assert new is None, "present already - nothing to write"
    assert summary["ours"] == [] and summary["theirs"] == 1
    assert net_compare_adr(theirs, favorites.sc_record(f"{ME}:27962", "x")), \
        "the engine's own dedupe sees ours and theirs as the same address"


def test_the_player_deleting_ours_brings_it_back_and_keeps_their_order():
    a = player_added("5.6.7.8:27960", "a")
    b = player_added("6.7.8.9:27960", "b")
    new, _ = favorites.sc_merge(with_favourites(empty_cache(), [a, b]), TA)
    assert [x for x, _, _ in lan_load_cached_servers(new)] == \
        ["5.6.7.8:27960", "6.7.8.9:27960", f"{ME}:27962"]


def test_a_full_list_is_reported_not_forced():
    full = [player_added("10.0.%d.%d:27960" % (i // 250, i % 250 + 1), "p")
            for i in range(128)]
    new, summary = favorites.sc_merge(with_favourites(empty_cache(), full), TA)
    assert new is None
    assert summary["full"] == [f"{ME}:27962"]


@pytest.mark.parametrize("mutate,why", [
    (lambda b: b[:-1], "bytes"),
    (lambda b: b + b"\0", "bytes"),
    (lambda b: struct.pack("<iii", 0, 0, 642048) + b[12:], "size field"),
    (lambda b: struct.pack("<iii", 0, 129, 692736) + b[12:], "numfavoriteservers"),
    (lambda b: struct.pack("<iii", -1, 0, 692736) + b[12:], "numglobalservers"),
    (lambda b: struct.pack("<iii", 4097, 0, 692736) + b[12:], "numglobalservers"),
])
def test_a_file_that_is_not_the_136_layout_is_refused(mutate, why):
    """LAN_LoadCachedServers checks only the size field and never bounds the
    counts; a file we do not understand is left exactly as it is."""
    with pytest.raises(favorites.ServerCacheError) as e:
        favorites.sc_merge(mutate(empty_cache()), TA)
    assert why in str(e.value)


def test_a_hostname_address_is_skipped_not_guessed():
    new, summary = favorites.sc_merge(None, [srow("myhost:27962", "missionpack",
                                                  local=1, source="local")])
    assert summary["skipped"] == ["myhost:27962"] and summary["ours"] == []


def test_the_staged_binary_is_the_layout_this_code_writes():
    """Pin the writer to the ioquake3.x86.exe the fleet actually runs: its own
    LAN_LoadCachedServers compares the size field with 0xA9200 and reads
    0xA4000 + 0x5200 bytes, and CL_SetServerInfo stores hostName at +0x20 and
    ping at +0x98 (disassembled 2026-09-29; the evidence file is
    .claude/evidence-1080p/build-favourites/servercache-layout-verification.txt).
    A different build restaged there must fail this, not silently get
    favourites in a layout it does not read."""
    need_share()
    exe = LIBRARY / "Quake3-TeamArena" / "ioquake3.x86.exe"
    data = exe.read_bytes()
    assert hashlib.md5(data).hexdigest() == "12f99bc0f9ba416ee477a4f13de8feaa", (
        "the staged ioquake3.x86.exe is not the build whose servercache.dat "
        "layout was verified - re-verify favorites.SC_* against it first")
    assert b"servercache.dat\x00" in data
    # cmp DWORD PTR [ebp-0x8],0xa9200
    assert bytes.fromhex("817df800920a00") in data
    # mov edx,0xa4000 ... mov ecx,0x5200 (the two array reads)
    assert bytes.fromhex("ba00400a00") in data and bytes.fromhex("b900520000") in data
    # CL_SetServerInfo: lea edx,[ebx+0x20] (hostName) / mov [ebx+0x98],esi (ping)
    assert bytes.fromhex("8d5320") in data and bytes.fromhex("89b398000000") in data


def ta_box(files=None, procs=(), with_missionpack=True, appdata_exists=True,
           **kw):
    home = r"C:\Documents and Settings\Administrator\Application Data"
    dirs = [Q3DIR + r"\baseq3"]
    if with_missionpack:
        dirs.append(Q3DIR + r"\missionpack")
    if appdata_exists:
        dirs.append(home + r"\Quake3\baseq3")
    else:
        dirs.append(home)
    f = {Q3CFG: STAGED_AUTOEXEC}
    f.update(files or {})
    return FakeBox(files=f, dirs=dirs, procs=procs, appdata=home, **kw), \
        home + r"\Quake3\servercache.dat"


def ta_notes(results):
    return [n for k, _, n in results if k == "missionpack"]


def test_push_seeds_team_arena_and_reads_it_back(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    box, path = ta_box(files={
        r"C:\Documents and Settings\Administrator\Application Data\Quake3"
        r"\servercache.dat": empty_cache()})
    res = push(con, box, monkeypatch)
    n = ta_notes(res)
    assert n and n[0].startswith("wrote 1 servers -> " + path), n
    assert [a for a, _, _ in lan_load_cached_servers(box.get(path))] == \
        [f"{ME}:27962"]
    assert box.sent.count(f"DOWNLOAD {path}") == 2, "read, then read back"
    # A second pass on the settled box writes nothing.
    box.sent.clear()
    n2 = ta_notes(push(con, box, monkeypatch))
    assert n2[0].startswith("unchanged: " + path), n2
    assert not [c for c in box.uploads() if "servercache" in c]


def test_team_arena_gets_only_team_arena_servers(con, monkeypatch):
    """A Team Arena client cannot join a baseq3 server, so unlike baseq3 the
    internet rows are filtered by gamename too (strict_accepts)."""
    local_q3(con)
    db.upsert_servers(con, "q3", [
        srow("8.8.8.8:27960", "baseq3", players=20),
        srow("7.7.7.7:27960", "missionpack", "Kr3m TA", players=2)])
    got = [r["addr"] for r in db.best_servers(
        con, "q3", accepts={"missionpack"}, strict_accepts=True)]
    assert got == [f"{ME}:27962", "7.7.7.7:27960"]
    # baseq3 keeps the permissive rule for the internet.
    base = [r["addr"] for r in db.best_servers(con, "q3", accepts={"baseq3"})]
    assert "8.8.8.8:27960" in base and f"{ME}:27962" not in base


@pytest.mark.parametrize("proc", ["ioquake3.x86.exe", "QUAKE3.EXE",
                                  "ioquake3.exe"])
def test_servercache_is_busy_while_any_quake3_client_runs(con, monkeypatch, proc):
    local_q3(con)
    index_q3(con)
    box, path = ta_box(procs=["explorer.exe", proc])
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith(f"BUSY: {path}: {proc.lower()} is running"), n
    assert "servercache" not in " ".join(box.uploads())


def test_servercache_is_not_written_when_proclist_cannot_answer(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    box, path = ta_box(proclist_fails=True)
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith("BUSY: ") and "PROCLIST failed" in n[0], n
    assert box.get(path) is None


def test_a_game_started_mid_pass_stops_the_write(con, monkeypatch):
    """PROCLIST is asked again right before the upload."""
    local_q3(con)
    index_q3(con)
    box, path = ta_box()
    calls = {"n": 0}
    orig = box.send_command

    async def racing(cmd, binary_payload=None, timeout=60):
        if cmd == "PROCLIST":
            calls["n"] += 1
            if calls["n"] >= 2:
                box.procs = ["ioquake3.x86.exe"]
        return await orig(cmd, binary_payload, timeout)
    box.send_command = racing
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith("BUSY: ") and "started while" in n[0], n
    assert box.get(path) is None


def test_no_missionpack_folder_means_no_team_arena(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    box, path = ta_box(with_missionpack=False)
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith("skipped: no missionpack folder"), n
    assert box.get(path) is None


def test_an_ioquake3_that_never_ran_is_not_seeded(con, monkeypatch):
    """No %APPDATA%\\Quake3 -> nothing is created; the folder is ioquake3's."""
    local_q3(con)
    index_q3(con)
    box, path = ta_box(appdata_exists=False)
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith("skipped: ") and "does not exist" in n[0], n
    assert not [c for c in box.sent if c.startswith("MKDIR")]
    assert box.get(path) is None


def test_a_missing_file_in_an_existing_homepath_is_created(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    box, path = ta_box()          # Quake3\baseq3 exists, no servercache.dat
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith("wrote 1 servers"), n
    assert lan_load_cached_servers(box.get(path))[0][0] == f"{ME}:27962"


def test_a_read_back_mismatch_is_a_failure_not_a_write(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    box, path = ta_box()
    orig = box.send_command

    async def tampering(cmd, binary_payload=None, timeout=60):
        st, data = await orig(cmd, binary_payload, timeout)
        if cmd.startswith("DOWNLOAD") and "servercache" in cmd and st == 1:
            return st, data[:-1] + b"\x01"
        return st, data
    box.send_command = tampering
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith("FAILED ") and "read back" in n[0], n
    assert db.applied_hash(con, "10.9.9.9", "missionpack",
                           path.rsplit("\\", 1)[0]) is None, "not recorded"


def test_an_unknown_layout_on_the_box_is_left_alone(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    retail = struct.pack("<iii", 0, 0, 4224 * 152) + bytes(4224 * 152)
    box, path = ta_box(files={
        r"C:\Documents and Settings\Administrator\Application Data\Quake3"
        r"\servercache.dat": retail})
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith("FAILED would clobber") and "1.36" in n[0], n
    assert box.get(path) == retail


def test_team_arena_policy_is_declared():
    pol = favorites.policy_for("missionpack")
    assert pol["supported"] and pol["engine"] == "q3cache"
    assert pol["accepts"] == {"missionpack"} and pol["strict_accepts"]
    assert pol["servers_from"] == "q3"
    assert pol["derived_from"] == ("ioquake3",)
    assert favorites.target_path("q3cache", Q3DIR, "missionpack") is None, \
        "it lives in the profile, not the tree"


# =============================================================================
# 3. BUSY per FILE, case-insensitively
# =============================================================================

def test_titles_sharing_a_file_are_planned_as_one_target():
    games = [{"game_key": "quake3", "dir": r"C:\GAMES\Quake3-TeamArena",
              "engine": "q3", "exe": r"C:\GAMES\Quake3-TeamArena\quake3.exe"},
             {"game_key": "ioquake3", "dir": r"C:\Games\Quake3-TeamArena",
              "engine": "q3", "exe": r"C:\Games\Quake3-TeamArena\IOQUAKE3.X86.EXE"},
             {"game_key": "ut99", "dir": r"C:\Games\UnrealTournament\System",
              "engine": "unreal", "exe": r"C:\Games\UnrealTournament\System\UnrealTournament.exe"},
             {"game_key": "starcraft", "dir": r"C:\Games\StarCraft",
              "engine": "-", "exe": r"C:\Games\StarCraft\StarCraft.exe"}]
    targets, skips = sync.plan_targets(games)
    q3 = [t for t in targets if t["engine"] == "q3"]
    assert len(q3) == 1, "one file, one decision - paths compare case-insensitively"
    assert q3[0]["keys"] == ["quake3", "ioquake3"]
    assert q3[0]["exes"] == {"quake3.exe", "ioquake3.x86.exe"}
    assert [k for k, _, _ in skips] == ["starcraft"]


def test_a_running_sibling_title_makes_the_shared_file_busy(con, monkeypatch):
    """The 00:04-00:24 journal on .123/.145/.195/.240: 'ioquake3 BUSY' then
    'quake3 wrote 16 servers' to the SAME file. Now the file is BUSY once."""
    local_q3(con)
    index_q3(con)
    box = FakeBox(files={Q3CFG: STAGED_AUTOEXEC}, dirs=[Q3DIR + r"\baseq3"],
                  procs=["ioquake3.x86.exe"], appdata=None)
    res = push(con, box, monkeypatch)
    q3 = [(k, n) for k, e, n in res if e == "q3"]
    assert q3 == [("ioquake3+quake3",
                   f"BUSY: {Q3CFG}: ioquake3.x86.exe is running - not attempted; "
                   f"it execs this file as it starts, so a write can land "
                   f"mid-launch; retry next pass")], q3
    assert f"UPLOAD {Q3CFG}" not in box.sent


def test_win98_upper_case_proclist_is_matched():
    """.243's PROCLIST: upper-case full paths. `\\.exe` alone matched none."""
    raw = json.dumps([{"pid": 1, "name": r"C:\WINDOWS\EXPLORER.EXE"},
                      {"pid": 2, "name": r"C:\GAMES\QUAKE2WIN9X\QUAKE2.EXE"},
                      {"pid": 3, "name": "Descent 3.exe"}])

    class C:
        async def command_text(self, cmd, timeout=30):
            return raw
    got = asyncio.run(sync.running_exes(C()))
    assert {"explorer.exe", "quake2.exe", "descent 3.exe"} <= got


def test_busy_reasons_name_the_file_kind():
    assert "execs this file" in favorites.busy_why("q3")
    assert "rewrites this ini" in favorites.busy_why("unreal")
    assert "ServerBrowser.vdf" in favorites.busy_why("goldsrc")
    assert "servercache.dat" in favorites.busy_why("q3cache")


def test_a_box_mid_gamesync_is_deferred_whole(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    box = FakeBox(files={Q3CFG: STAGED_AUTOEXEC}, dirs=[Q3DIR + r"\baseq3"],
                  gamesync="copying")
    res = push(con, box, monkeypatch)
    assert notes(res) == ["BUSY: GAMESYNC is copying on this box - it copies "
                          "the staged files back over ours; deferred to the "
                          "next pass"]
    assert not box.uploads()


# =============================================================================
# FAV-2: nothing is created above a title's own folder
# =============================================================================

def test_dirlist_of_a_missing_folder_is_not_evidence_the_folder_exists():
    """files.c answers [] for a folder that does not exist. The old code read
    that as "folder there, file missing" and MKDIR'd a phantom tree."""
    box = FakeBox(dirs=[r"C:\Games\JediAcademy\base"])
    text, state, why = asyncio.run(sync.read_existing(
        box, r"C:\Games\JediAcademy\baseq3\autoexec.cfg"))
    assert state == "no-parent" and "baseq3 does not exist" in why


def test_an_empty_folder_that_exists_is_still_missing_not_absent():
    box = FakeBox(dirs=[r"C:\Games\Q\baseq3"])
    assert asyncio.run(sync.read_existing(
        box, r"C:\Games\Q\baseq3\autoexec.cfg"))[1] == "missing"


def test_push_never_mkdirs_and_skips_a_purged_title(con, monkeypatch):
    db.upsert_servers(con, "q3", [srow(f"{ME}:27961", "baseq3", local=1,
                                       source="local")])
    db.replace_games(con, "10.9.9.9", [{"key": "quake3", "dir": r"C:\Games\Purged",
                                        "engine": "q3", "exe": "quake3.exe"}])
    box = FakeBox(dirs=[r"C:\Games"], appdata=None)
    res = push(con, box, monkeypatch)
    assert any("does not exist" in n and "nothing is ever created" in n
               for n in notes(res)), notes(res)
    assert not [c for c in box.sent if c.startswith(("MKDIR", "UPLOAD"))]
    src = (REPO / "scripts" / "gameindex" / "sync.py").read_text()
    assert 'send_command(f"MKDIR' not in src, "MKDIR is back in the push"


# =============================================================================
# 4. a settled box says "unchanged"
# =============================================================================

def test_the_same_servers_in_another_order_and_other_labels_is_unchanged():
    a = [srow("1.1.1.1:27960", name="one"), srow("2.2.2.2:27960", name="two")]
    b = [srow("2.2.2.2:27960", name="TWO ^1renamed"), srow("1.1.1.1:27960", name="1")]
    ta_, _ = favorites.render("q3", a, STAGED_AUTOEXEC, key="quake3")
    tb, _ = favorites.render("q3", b, "", key="quake3")
    tb = STAGED_AUTOEXEC.rstrip("\n") + "\n\n" + tb
    assert ta_ != tb
    assert favorites.same_favourites("q3", ta_, tb)


def test_a_different_server_is_still_a_change():
    a = [srow("1.1.1.1:27960")]
    b = [srow("3.3.3.3:27960")]
    ta_, _ = favorites.render("q3", a, STAGED_AUTOEXEC, key="quake3")
    tb, _ = favorites.render("q3", b, STAGED_AUTOEXEC, key="quake3")
    assert not favorites.same_favourites("q3", ta_, tb)


def test_a_foreign_line_change_is_still_a_change():
    a = [srow("1.1.1.1:27960")]
    ta_, _ = favorites.render("q3", a, STAGED_AUTOEXEC, key="quake3")
    tb = ta_.replace('seta com_maxfps "125"', 'seta com_maxfps "90"')
    assert not favorites.same_favourites("q3", ta_, tb)


def test_one_server_leaving_changes_one_line_not_every_line_after_it():
    """The 09-29 00:04 -> 00:28 diff on .240: server3..server14 all rewritten
    because one server changed and the list was renumbered."""
    servers = [srow(f"10.0.0.{i}:27960") for i in range(1, 16)]
    first, _ = favorites.render("q3", servers, STAGED_AUTOEXEC, key="quake3")
    changed = [s for s in servers if s["addr"] != "10.0.0.3:27960"] + \
        [srow("10.0.0.99:27960")]
    second, _ = favorites.render("q3", changed, first, key="quake3")
    diff = [(x, y) for x, y in zip(first.splitlines(), second.splitlines()) if x != y]
    assert len(diff) == 1 and "10.0.0.3:27960" in diff[0][0] \
        and "10.0.0.99:27960" in diff[0][1], diff


def test_unreal_keeps_the_existing_order_of_what_it_keeps():
    s1 = srow("1.1.1.1:7777", "ut", source="seed", query_port=7778)
    s2 = srow("2.2.2.2:7777", "ut", source="seed", query_port=7778)
    s3 = srow("3.3.3.3:7777", "ut", source="seed", query_port=7778)
    base = "[UBrowser.UBrowserFavoritesFact]\nFavoriteCount=0\nFavorites[0]=\n"
    first, _ = favorites.render("unreal", [s2, s1], base, key="ut99")
    assert first.index("2.2.2.2") < first.index("1.1.1.1")
    second, _ = favorites.render("unreal", [s1, s2, s3], first, key="ut99")
    assert second.index("2.2.2.2") < second.index("1.1.1.1") < second.index("3.3.3.3")


def test_cs_resaving_its_own_vdf_is_not_a_change():
    """revSrvBrowser writes `"address"` with ONE tab and a real lastplayed. The
    old writer used two tabs and "0", so every CS exit meant a rewrite."""
    cs = [srow(f"{ME}:27015", "cstrike", "NSC CS", local=1, source="local")]
    empty = '"filters"\n{\n\t"favorites"\n\t{\n\t}\n\n\t"history"\n\t{\n\t}\n\n}\n'
    ours, _ = favorites.render("goldsrc", cs, empty, key="cs16")
    assert '\t\t\t"address"\t"192.168.1.132:27015"' in ours, \
        "one tab, as the DLL's own template"
    resaved = ours.replace('"lastplayed"\t"0"', '"lastplayed"\t"1759123456"') \
        .replace('"NSC CS"', '"NSC Retro Fleet Arena (CS 1.6)"')
    again, _ = favorites.render("goldsrc", cs, resaved, key="cs16")
    assert favorites.same_favourites("goldsrc", resaved, again)


def test_a_rewrite_keeps_the_lastplayed_the_game_recorded():
    """When the vdf must change (a server was added), a favourite the player
    has used keeps its stamp instead of being reset to 0."""
    cs = [srow(f"{ME}:27015", "cstrike", "NSC CS", local=1, source="local")]
    empty = '"filters"\n{\n\t"favorites"\n\t{\n\t}\n\n\t"history"\n\t{\n\t}\n\n}\n'
    ours, _ = favorites.render("goldsrc", cs, empty, key="cs16")
    played = ours.replace('"lastplayed"\t"0"', '"lastplayed"\t"1759123456"')
    more = cs + [srow(f"{ME}:27016", "cstrike", "NSC CS no-blood", local=1,
                      source="local")]
    text, _ = favorites.render("goldsrc", more, played, key="cs16")
    assert not favorites.same_favourites("goldsrc", played, text)
    assert '"lastplayed"\t"1759123456"' in text and text.count('"lastplayed"\t"0"') == 1
    # and it is still stable: rendering over its own output is a no-op
    again, _ = favorites.render("goldsrc", more, text, key="cs16")
    assert again.splitlines() == text.splitlines()


def test_seeds_on_one_host_are_both_kept(con):
    """85.214.243.170:7777 and :9000 are two curated servers. Deduping them by
    host kept one, chosen by a 2.5 s read timeout - UT99 flipped every pass."""
    db.upsert_servers(con, "unreal", [
        srow("85.214.243.170:7777", "ut", "Monsterhunt World", 0, 0, "seed",
             ping=2633, query_port=7778),
        srow("85.214.243.170:9000", "ut", "Monsterhunt Tank World", 0, 0, "seed",
             ping=2659, query_port=9001)])
    got = {r["addr"] for r in db.best_servers(con, "unreal", limit=24)}
    assert got == {"85.214.243.170:7777", "85.214.243.170:9000"}


def test_the_ut99_render_no_longer_flips_with_the_ping_bands(con):
    renders = set()
    for pings in ((2633, 2659), (2659, 2633), (2699, 2601)):
        db.upsert_servers(con, "unreal", [
            srow("85.214.243.170:7777", "ut", "World", 0, 0, "seed",
                 ping=pings[0], query_port=7778),
            srow("85.214.243.170:9000", "ut", "Tank", 0, 0, "seed",
                 ping=pings[1], query_port=9001)])
        servers = db.best_servers(con, "unreal", limit=24, accepts={"ut"})
        renders.add(favorites.render("unreal", servers, "", key="ut99")[0])
    assert len(renders) == 1


def test_an_incumbent_stays_ahead_of_a_busier_newcomer(con):
    db.upsert_servers(con, "q3", [srow("1.1.1.1:27960", players=1),
                                  srow("2.2.2.2:27960", players=30)])
    assert [r["addr"] for r in db.best_servers(con, "q3", limit=1)] == \
        ["2.2.2.2:27960"]
    inc = [r["addr"] for r in db.best_servers(
        con, "q3", limit=1, incumbent=lambda r: r["addr"] == "1.1.1.1:27960")]
    assert inc == ["1.1.1.1:27960"], "the box keeps what it lists while it lives"


def test_an_incumbent_that_emptied_or_died_is_replaced(con):
    db.upsert_servers(con, "q3", [srow("1.1.1.1:27960", players=0),
                                  srow("2.2.2.2:27960", players=5)])
    got = [r["addr"] for r in db.best_servers(
        con, "q3", limit=1, incumbent=lambda r: r["addr"] == "1.1.1.1:27960")]
    assert got == ["2.2.2.2:27960"]


def test_an_incumbent_port_holds_its_host_against_a_sibling_port(con):
    """155.138.197.166 runs 2020..2027; the pick flipped 2024<->2020 on
    player jitter. The port the box lists holds the host."""
    db.upsert_servers(con, "q3", [srow("155.138.197.166:2020", players=20),
                                  srow("155.138.197.166:2024", players=4)])
    got = [r["addr"] for r in db.best_servers(
        con, "q3", incumbent=lambda r: r["addr"] == "155.138.197.166:2024")]
    assert got == ["155.138.197.166:2024"]


def test_settled_box_end_to_end_reports_unchanged(con, monkeypatch):
    """Pass 1 writes; the ranking then reshuffles and the labels change; pass 2
    must say "unchanged" for every file and upload nothing."""
    local_q3(con)
    db.upsert_servers(con, "q3", [srow(f"10.0.0.{i}:27960", players=4 + i)
                                  for i in range(1, 25)])
    index_q3(con)
    box = FakeBox(files={Q3CFG: STAGED_AUTOEXEC}, dirs=[Q3DIR + r"\baseq3"],
                  appdata=None)
    first = push(con, box, monkeypatch)
    assert any(n.startswith("wrote 16 servers") for n in notes(first)), notes(first)
    # the world moves: new busiest servers, renamed ones, reversed ranking
    db.upsert_servers(con, "q3", [srow(f"10.0.0.{i}:27960", name=f"renamed {i}",
                                       players=40 - i) for i in range(1, 25)] +
                      [srow(f"10.0.1.{i}:27960", players=60) for i in range(1, 5)])
    box.sent.clear()
    second = push(con, box, monkeypatch)
    q3 = [n for k, e, n in second if e == "q3"]
    assert q3 and q3[0].startswith(f"unchanged: {Q3CFG}"), q3
    assert not box.uploads()


def test_a_reverted_file_is_still_repaired(con, monkeypatch):
    """GAMESYNC copying the staged file back is a REAL difference."""
    local_q3(con)
    index_q3(con)
    box = FakeBox(files={Q3CFG: STAGED_AUTOEXEC}, dirs=[Q3DIR + r"\baseq3"],
                  appdata=None)
    push(con, box, monkeypatch)
    box.put(Q3CFG, STAGED_AUTOEXEC)             # GAMESYNC restored the library copy
    res = push(con, box, monkeypatch)
    assert any(n.startswith("wrote") for k, e, n in res if e == "q3")
    assert "192.168.1.132:27961" in box.get(Q3CFG).decode("latin-1")


# --- humans, not bots; and a ping that is a ping -----------------------------

def test_bots_are_not_players():
    lines = ['12 0 "Sarge"', '5 0 "Major"', '9 48 "a person"', '0 999 "joining"']
    assert masters._humans(lines) == 2


def test_the_q3_probe_counts_humans(monkeypatch):
    reply = (b"\xff\xff\xff\xffstatusResponse\n"
             b"\\sv_hostname\\bots r us\\mapname\\q3dm17\\sv_maxclients\\16\n"
             b'3 0 "Sarge"\n4 0 "Doom"\n7 0 "Hunter"\n')
    monkeypatch.setattr(masters, "_udp", lambda *a, **k: (reply, 40))
    row = masters._q3_probe("1.2.3.4:27960")
    assert row["players"] == 0 and row["bots"] == 3
    reply2 = reply + b'2 57 "human"\n'
    monkeypatch.setattr(masters, "_udp", lambda *a, **k: (reply2, 40))
    assert masters._q3_probe("1.2.3.4:27960")["players"] == 1


def test_the_rtt_is_the_first_reply_not_the_last_timeout():
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]

    def answer_once():
        data, addr = srv.recvfrom(100)
        srv.sendto(b"\\hostname\\x\\final\\", addr)
    t = threading.Thread(target=answer_once, daemon=True)
    t.start()
    data, rtt = masters._udp("127.0.0.1", port, b"\\status\\", timeout=0.6, reads=3)
    srv.close()
    assert data and rtt < 300, f"rtt {rtt} ms is the second read's timeout"


# --- bytes >= 0x80 survive the merge -----------------------------------------

def test_a_high_byte_in_a_config_survives_a_merge(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    staged = STAGED_AUTOEXEC.encode("latin-1") + b"// Caf\xe9 \xbb\n"
    box = FakeBox(files={Q3CFG: staged}, dirs=[Q3DIR + r"\baseq3"], appdata=None)
    push(con, box, monkeypatch)
    out = box.get(Q3CFG)
    assert b"// Caf\xe9 \xbb" in out, "latin-1 in, latin-1 out - never '?'"
    box.sent.clear()
    res = push(con, box, monkeypatch)
    assert [n for k, e, n in res if e == "q3"][0].startswith("unchanged")


# --- a stale index is not written from; a lost address fails the pass --------

def test_a_box_that_could_not_be_reindexed_is_not_written(con, tmp_path, monkeypatch):
    monkeypatch.setattr(db, "DB_PATH", str(tmp_path / "t.db"))
    monkeypatch.setattr(sync, "live_agents", lambda: ["10.9.9.9"])

    async def refresh(_con, ip, force=False):
        return False, None, "ERROR ConnectionRefusedError: [Errno 111] refused"
    monkeypatch.setattr(sync, "refresh_machine", refresh)
    monkeypatch.setattr(sync, "refresh_servers", lambda c, e: {})
    monkeypatch.setattr(sync, "probe_local_servers", lambda c: (20, []))
    called = []

    async def must_not(*a, **k):
        called.append(a)
        return []
    monkeypatch.setattr(sync, "push_favorites", must_not)
    report = asyncio.run(sync.run_once())
    assert not called
    assert report["writes"]["skipped"] == 1 and report["ok"] is True


def test_every_local_server_silent_on_an_address_we_do_not_hold_fails_the_pass(
        tmp_path, monkeypatch):
    monkeypatch.setattr(db, "DB_PATH", str(tmp_path / "t.db"))
    monkeypatch.setattr(sync, "ME", "192.0.2.77")        # TEST-NET-1: never ours
    monkeypatch.setattr(sync, "live_agents", lambda: [])
    monkeypatch.setattr(sync, "refresh_servers", lambda c, e: {})
    monkeypatch.setattr(sync, "probe_local_servers",
                        lambda c: (len(sync.LOCAL_SERVERS),
                                   ["q3:%d" % i for i in range(len(sync.LOCAL_SERVERS))]))
    report = asyncio.run(sync.run_once())
    assert report["ok"] is False
    assert "does not hold 192.0.2.77" in report["errors"][0]


def test_host_holds_address():
    assert sync.host_holds_address("127.0.0.1") is True
    assert sync.host_holds_address("192.0.2.77") is False


# =============================================================================
# 5. reasons that are true, and writers that reached nothing
# =============================================================================

@pytest.mark.parametrize("key,addr", [
    ("jka", "192.168.1.132:29070"), ("descent3", "192.168.1.132:2092"),
    ("tribes2", "192.168.1.132:28000"), ("hl2", "192.168.1.132:27025"),
    ("wolfmp", "192.168.1.132:27963"), ("deusex", "192.168.1.132:7790"),
    ("doom3", "192.168.1.132:27666"), ("halflife", "192.168.1.132:27020"),
    ("sof2", "192.168.1.132:20100")])
def test_a_title_with_a_fleet_server_is_never_told_there_is_none(key, addr):
    why = favorites.policy_for(key)["why"]
    assert addr in why, key
    assert not re.search(r"no fleet [\w ]*server|there is no fleet", why, re.I), \
        f"{key}: a reason still denies a server that exists"


def test_the_jka_reason_is_no_longer_the_cd_check():
    why = favorites.UNWRITABLE["jka"]
    assert "cannot get past" not in why and "Disk 1" not in why
    assert "servercache.dat" in why and "disc image" in why


def test_rtcw_and_deus_ex_are_no_longer_reported_as_written():
    """Both wrote a file every pass that no screen of theirs reads."""
    for key, mechanism in (("wolfmp", "servercache.dat"),
                           ("deusex", "UBrowserFavoritesFact")):
        pol = favorites.policy_for(key)
        assert not pol["supported"], key
        assert mechanism in pol["why"], key
        assert key not in favorites.TITLES


def test_tribes2_is_not_said_to_have_no_favourites():
    why = favorites.UNWRITABLE["tribes2"]
    assert "$pref::ServerBrowser::Favorite" in why
    assert "keeps no favourites file" not in why


@pytest.mark.parametrize("key", ["bf1942", "heretic", "hexen", "doom3", "farcry"])
def test_titles_that_fell_through_now_have_an_answer(key):
    why = favorites.policy_for(key)["why"]
    assert "no verified favourites mechanism" not in why
    assert len(why) > 60


def _library_keys():
    """Every GAMEINDEX key the agent would detect in each staged title, by
    walking the tree the way gameindex.c does (exe names, case-insensitive,
    a moddir for hl.exe)."""
    from test_gameindex_staged_library import signature_rows
    sigs = signature_rows()
    by_exe = {}
    for r in sigs:
        by_exe.setdefault(r["exe"].lower(), []).append(r)
    found = {}
    for title in sorted(p for p in LIBRARY.iterdir()
                        if p.is_dir() and not p.name.startswith("_")):
        keys = set()
        for root, dirs, files in os.walk(title):
            # gameindex.c walks C:\Games at depth 0 and stops descending at
            # GI_MAX_DEPTH 3: a title's own folder is depth 1, so it sees
            # exes down to <title>\<a>\<b>\ and no deeper. Mirror that.
            depth = len(pathlib.Path(root).relative_to(title).parts)
            if depth >= 2:
                dirs[:] = []
            lower_dirs = {d.lower() for d in dirs}
            for f in files:
                for r in by_exe.get(f.lower(), []):
                    if r["moddir"] is None or r["moddir"].lower() in lower_dirs:
                        keys.add(r["key"])
        found[title.name] = keys
    return found


def test_every_key_detected_in_the_staged_library_has_an_explicit_answer():
    """Built from the LIBRARY, not a hand-kept list: the 33-title list missed
    19 of 52 titles, BF1942 among them, and so never noticed it falling
    through to the generic reason."""
    need_share()
    found = _library_keys()
    assert len(found) >= 40, f"only {len(found)} titles - is the share complete?"
    unanswered = sorted(
        f"{title}: {key}" for title, keys in found.items() for key in keys
        if "no verified favourites mechanism" in
        (favorites.policy_for(key).get("why") or ""))
    assert not unanswered, "detected but never looked at: " + ", ".join(unanswered)
    undetected = sorted(t for t, k in found.items() if not k)
    print("staged titles GAMEINDEX cannot see (no signature): "
          + ", ".join(undetected))


def test_the_staged_wolfmp_ui_really_has_no_server_format():
    """The evidence behind moving wolfmp: its multiplayer UI never formats
    server%d, and names the engine favourites verbs instead."""
    need_share()
    dll = (LIBRARY / "ReturnToCastleWolfenstein" / "Main" / "ui_mp_x86.dll").read_bytes()
    assert b"server%d" not in dll
    assert b"addFavorite" in dll and b"createFavorite" in dll
    assert b"servercache.dat" in \
        (LIBRARY / "ReturnToCastleWolfenstein" / "WolfMP.exe").read_bytes()


def test_the_staged_won_half_life_is_protocol_45():
    need_share()
    hw = (LIBRARY / "HalfLife1" / "hw.dll").read_bytes()
    s = b"CL_Parse_Version: Server is protocol %i instead of %i"
    i = hw.find(s)
    assert i > 0
    # the push of that string's address follows `cmp eax,2Dh; je; push 2Dh`
    assert bytes.fromhex("83f82d74106a2d50") in hw


# =============================================================================
# 6. adversarial review, 2026-09-29 - two defects found after the build
# =============================================================================
#
# (a) Team Arena: a fleet server the merge could NOT put in the list (the
#     player's 128 favourites fill it, or the address is a hostname) was
#     reported "unchanged: ... ours: none" - success, with the server absent
#     from the Favorites tab and nothing saying so. And with no file yet the
#     writer created an EMPTY servercache.dat for a list it could not fill.
# (b) Q3 favourites still churned on real data: replaying the captured box
#     files against two DB snapshots 20 minutes apart (05:04 / 05:24) rewrote
#     every Q3 autoexec on every LCD box, because two incumbents had simply
#     not been re-probed in the last 15 minutes (what the masters return
#     varies pass to pass - 389 addresses at 05:34, 862 either side - and a
#     third of it does not answer any one probe) and dropped out, and a
#     sibling port of the same host took the slot. An incumbent is now judged
#     alive over the hour prune_servers keeps a row for.

SC_PATH = (r"C:\Documents and Settings\Administrator\Application Data\Quake3"
           r"\servercache.dat")


def test_a_full_list_is_reported_as_not_added_never_as_unchanged(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    full = [player_added("10.0.%d.%d:27960" % (i // 250, i % 250 + 1), "p")
            for i in range(128)]
    box, path = ta_box(files={SC_PATH: with_favourites(empty_cache(), full)})
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith(f"skipped: {path}: NOT ADDED {ME}:27962 - the "
                           f"favourites list is full"), n
    assert not n[0].startswith("unchanged")
    assert not [c for c in box.uploads() if "servercache" in c]
    assert box.get(path) == with_favourites(empty_cache(), full), "untouched"


def test_an_address_the_file_cannot_hold_is_reported_and_no_empty_file_made(
        con, monkeypatch):
    """RETRO_FLEET_HOST may name the host; servercache.dat holds 4 IP bytes."""
    db.upsert_servers(con, "q3", [srow("fleethost:27962", "missionpack",
                                       "TA", 0, 1, "local")])
    index_q3(con)
    box, path = ta_box()              # Quake3\ exists, no servercache.dat yet
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith(f"skipped: {path}: NOT ADDED fleethost:27962 - "
                           f"not an IPv4"), n
    assert box.get(path) is None, \
        "an empty servercache.dat seeded for a list we could not fill"


def test_sc_merge_creates_no_file_with_nothing_of_ours_to_put_in_it():
    new, summary = favorites.sc_merge(None, [srow("myhost:27962", "missionpack",
                                                  local=1, source="local")])
    assert new is None
    assert "NOT ADDED myhost:27962" in favorites.sc_missing(summary)
    # ...while a representable one still creates the file, as before
    new, summary = favorites.sc_merge(None, TA)
    assert new is not None and favorites.sc_missing(summary) == ""


def test_the_players_own_copy_of_the_fleet_server_is_named(con, monkeypatch):
    local_q3(con)
    index_q3(con)
    theirs = player_added(f"{ME}:27962", "added by hand")
    box, path = ta_box(files={SC_PATH: with_favourites(empty_cache(), [theirs])})
    n = ta_notes(push(con, box, monkeypatch))
    assert n[0].startswith(f"unchanged: {path}"), n
    assert f"already the player's own: {ME}:27962" in n[0], n


def _age(con, addr, minutes, engine="q3"):
    t = time.strftime("%Y-%m-%d %H:%M:%S",
                      time.localtime(time.time() - minutes * 60))
    con.execute("UPDATE servers SET last_seen=? WHERE engine=? AND addr=?",
                (t, engine, addr))
    con.commit()


def test_an_incumbent_a_pass_missed_is_kept_but_a_newcomer_that_old_is_not(con):
    db.upsert_servers(con, "q3", [srow("1.1.1.1:27960", players=4),
                                  srow("2.2.2.2:27960", players=30)])
    _age(con, "1.1.1.1:27960", 30)
    _age(con, "2.2.2.2:27960", 30)
    inc = [r["addr"] for r in db.best_servers(
        con, "q3", incumbent=lambda r: r["addr"] == "1.1.1.1:27960")]
    assert inc == ["1.1.1.1:27960"], "listed, alive half an hour ago: kept"
    assert db.best_servers(con, "q3") == [], \
        "a server nobody lists must have been seen in the last 15 minutes"


def test_an_incumbent_gone_for_an_hour_or_last_seen_empty_is_replaced(con):
    db.upsert_servers(con, "q3", [srow("1.1.1.1:27960", players=4),
                                  srow("3.3.3.3:27960", players=0),
                                  srow("2.2.2.2:27960", players=6)])
    _age(con, "1.1.1.1:27960", 70)             # past the prune horizon
    _age(con, "3.3.3.3:27960", 20)             # answered - empty
    listed = {"1.1.1.1:27960", "3.3.3.3:27960"}
    got = [r["addr"] for r in db.best_servers(
        con, "q3", incumbent=lambda r: r["addr"] in listed)]
    assert got == ["2.2.2.2:27960"]


def test_a_pass_that_missed_listed_servers_does_not_rewrite_the_file(
        con, monkeypatch):
    """The 05:04 -> 05:24 replay, in miniature: five of the listed internet
    servers were not re-probed by the latest discovery, so their rows are 25
    minutes old. Nothing a player could see changed - the file must not."""
    local_q3(con)
    db.upsert_servers(con, "q3", [srow(f"10.0.0.{i}:27960", players=8)
                                  for i in range(1, 25)])
    index_q3(con)
    box = FakeBox(files={Q3CFG: STAGED_AUTOEXEC}, dirs=[Q3DIR + r"\baseq3"],
                  appdata=None)
    first = push(con, box, monkeypatch)
    assert any(n.startswith("wrote 16 servers") for n in notes(first))
    listed = favorites.incumbents("q3", box.get(Q3CFG).decode("latin-1"))
    missed = sorted(listed - {f"{ME}:27961"})[:5]
    for a in missed:
        _age(con, a, 25)
    box.sent.clear()
    second = push(con, box, monkeypatch)
    q3 = [n for k, e, n in second if e == "q3"]
    assert q3 and q3[0].startswith(f"unchanged: {Q3CFG}"), q3
    assert not box.uploads()
    # ...and once they have really gone (an hour unseen) the file follows.
    for a in missed:
        _age(con, a, 70)
    third = push(con, box, monkeypatch)
    q3 = [n for k, e, n in third if e == "q3"]
    assert q3 and q3[0].startswith("wrote 16 servers"), q3
    now = favorites.incumbents("q3", box.get(Q3CFG).decode("latin-1"))
    assert not (set(missed) & now)


def test_the_sof2_reason_matches_the_staged_tree():
    """The SoF2 reason said "the staged tree carries base\\servercache.dat".
    It does not (case-insensitive, whole tree): the game writes the file at
    its tree root on the boxes. The mechanism half of the reason stands - the
    MP UI is Team Arena's lineage, addFavorite and no server%d."""
    need_share()
    tree = LIBRARY / "SoldierOfFortune2"
    staged = [p for p in tree.rglob("*") if p.name.lower() == "servercache.dat"]
    assert staged == [], staged
    why = favorites.UNWRITABLE["sof2"]
    assert "staged tree carries" not in why and "none is staged" in why
    import zipfile
    ui = zipfile.ZipFile(tree / "base" / "mp.pk3").read("vm/sof2mp_ui.qvm")
    assert b"addFavorite" in ui and b"server%d" not in ui
    assert b"servercache.dat" in (tree / "sof2mp.exe").read_bytes()
