"""Regression: retro_playerprofile.py - player profiles + game configs.

Locks in the contract the player-profile skill relies on:
  * create/set/game-set/bind round-trip through the DB,
  * the cvar command word is PRESERVED per game family (writing `seta` into a
    GoldSrc userconfig.cfg is a syntax error; dropping it from q3config.cfg
    loses the archive flag),
  * config parsing ignores comments and the bind/unbindall verbs rather than
    storing them as cvars,
  * an ini profile is applied as a PATCH - unrelated engine keys in the game's
    own ini survive, which is the difference between a working game and a
    broken one,
  * apply --dry-run never touches the network.
"""
import importlib.util
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
PP_PATH = os.path.abspath(os.path.join(HERE, "..", "..", "scripts",
                                       "retro_playerprofile.py"))


def _pp(tmp_path):
    os.environ["RETRO_PLAYERS_DB"] = str(tmp_path / "players.db")
    spec = importlib.util.spec_from_file_location("retro_playerprofile",
                                                  PP_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    # module reads the env var at import time
    assert mod.DB_PATH == str(tmp_path / "players.db")
    return mod


def test_create_set_show_roundtrip(tmp_path, capsys):
    pp = _pp(tmp_path)
    pp.main(["create", "voidsstr", "--name", "Void"])
    pp.main(["set", "voidsstr", "mouse_dpi=800", "vsync=on"])
    pp.main(["game-set", "voidsstr", "quake3", "cg_fov=110", "sensitivity=3.5"])
    pp.main(["bind", "voidsstr", "quake3", "MOUSE2", "+zoom"])
    capsys.readouterr()
    pp.main(["show", "voidsstr"])
    out = capsys.readouterr().out
    assert "mouse_dpi" in out and "800" in out
    assert "quake3" in out and "2 settings" in out and "1 binds" in out


def test_handle_is_case_insensitive(tmp_path, capsys):
    pp = _pp(tmp_path)
    pp.main(["create", "VoidSstr"])
    capsys.readouterr()
    pp.main(["show", "VOIDSSTR"])
    assert "player:   voidsstr" in capsys.readouterr().out


def test_unknown_player_exits_rather_than_creating_one(tmp_path):
    pp = _pp(tmp_path)
    with pytest.raises(SystemExit):
        pp.main(["game-set", "ghost", "quake3", "cg_fov=110"])


def test_quake_parse_skips_comments_and_verbs(tmp_path):
    pp = _pp(tmp_path)
    text = ('// my config\r\n'
            'seta cg_fov "110"\r\n'
            'bind w "+forward"\r\n'
            'unbindall\r\n'
            'exec autoexec.cfg\r\n'
            '\r\n')
    settings, binds = pp.parse_quake(text)
    assert settings == {"cg_fov": ("110", "seta")}
    assert binds == {"w": "+forward"}
    # the verbs must NOT have become cvars
    assert "unbindall" not in settings and "exec" not in settings
    assert "bind" not in settings


def test_cvar_command_word_is_preserved_per_family(tmp_path, capsys):
    """q3config.cfg uses `seta`; a GoldSrc userconfig.cfg uses a bare line.

    Emitting the wrong one breaks the game silently, so the parsed command
    word round-trips instead of being normalized.
    """
    pp = _pp(tmp_path)
    pp.main(["create", "p"])
    q3 = tmp_path / "q3config.cfg"
    q3.write_bytes(b'seta cg_fov "110"\r\n')
    gs = tmp_path / "userconfig.cfg"
    gs.write_bytes(b'rate "25000"\r\n')
    pp.main(["import", "p", "quake3", "--file", str(q3)])
    pp.main(["import", "p", "cs16", "--file", str(gs)])
    capsys.readouterr()
    pp.main(["render", "p", "quake3"])
    assert 'seta cg_fov "110"' in capsys.readouterr().out
    pp.main(["render", "p", "cs16"])
    out = capsys.readouterr().out
    assert 'rate "25000"' in out
    assert "seta rate" not in out and "set rate" not in out


def test_render_uses_crlf(tmp_path, capsys):
    """DOS/Win9x game parsers are unhappy with bare LF configs."""
    pp = _pp(tmp_path)
    pp.main(["create", "p"])
    pp.main(["game-set", "p", "quake3", "cg_fov=110"])
    out = tmp_path / "out.cfg"
    pp.main(["render", "p", "quake3", "--out", str(out)])
    raw = out.read_bytes()
    assert b"\r\n" in raw
    assert b"\n" not in raw.replace(b"\r\n", b"")


def test_ini_patch_preserves_unrelated_keys(tmp_path):
    pp = _pp(tmp_path)
    orig = ("[WinDrv.WindowsClient]\r\n"
            "WindowedViewportX=640\r\n"
            "WindowedViewportY=480\r\n"
            "\r\n"
            "[Engine.GameEngine]\r\n"
            "ServerActors=IpDrv.MasterServerUplink\r\n")
    out = pp.apply_ini_patch(orig, [
        ("WinDrv.WindowsClient.WindowedViewportX", "1024", ""),
        ("Engine.GameEngine.CacheSizeMegs", "64", ""),
    ])
    assert "WindowedViewportX=1024" in out
    # untouched keys survive - this is the whole point of a patch
    assert "WindowedViewportY=480" in out
    assert "ServerActors=IpDrv.MasterServerUplink" in out
    # a new key lands inside its own section, not at the end of the file
    lines = [l.strip() for l in out.splitlines()]
    assert lines.index("CacheSizeMegs=64") > lines.index("[Engine.GameEngine]")
    # and it is not duplicated
    assert out.count("WindowedViewportX=") == 1


def test_ini_patch_adds_missing_section(tmp_path):
    pp = _pp(tmp_path)
    out = pp.apply_ini_patch("[A]\r\nx=1\r\n",
                             [("B.y", "2", "")])
    assert "[A]" in out and "x=1" in out
    assert "[B]" in out and "y=2" in out


def test_apply_dry_run_makes_no_network_call(tmp_path, capsys, monkeypatch):
    pp = _pp(tmp_path)
    pp.main(["create", "p"])
    pp.main(["game-set", "p", "quake3", "cg_fov=110"])
    capsys.readouterr()

    def boom(*args, **kwargs):
        raise AssertionError("dry run must not touch the box")
    monkeypatch.setattr(pp, "_agent_call", boom)
    pp.main(["apply", "p", "quake3", "--host", "192.168.1.185", "--dry-run"])
    out = capsys.readouterr().out
    assert "would write to 192.168.1.185" in out
    assert 'seta cg_fov "110"' in out


def test_apply_empty_profile_refuses(tmp_path, monkeypatch):
    """Applying an empty profile would blank the box's config file."""
    pp = _pp(tmp_path)
    pp.main(["create", "p"])
    pp.get_profile(pp.connect(), pp.get_player(pp.connect(), "p")["id"],
                   "quake3", create=True)

    def boom(*args, **kwargs):
        raise AssertionError("must not touch the box")
    monkeypatch.setattr(pp, "_agent_call", boom)
    with pytest.raises(SystemExit):
        pp.main(["apply", "p", "quake3", "--host", "192.168.1.185"])


def test_capture_and_apply_use_the_agent(tmp_path, capsys, monkeypatch):
    """capture parses what the box returns; apply writes the rendered config
    and archives whatever was there before, so the push is reversible."""
    pp = _pp(tmp_path)
    pp.main(["create", "p"])
    capsys.readouterr()
    monkeypatch.setattr(pp, "agent_read_file",
                        lambda h, path: 'seta cg_fov "110"\r\n')
    pp.main(["capture", "p", "quake3", "--host", "192.168.1.133"])
    assert "captured 1 settings" in capsys.readouterr().out

    written = {}
    monkeypatch.setattr(pp, "agent_read_file",
                        lambda h, path: 'seta cg_fov "90"\r\n')
    monkeypatch.setattr(pp, "agent_write_file",
                        lambda h, path, text: written.update(
                            host=h, path=path, text=text))
    pp.main(["apply", "p", "quake3", "--host", "192.168.1.185"])
    capsys.readouterr()
    assert written["host"] == "192.168.1.185"
    assert written["path"].endswith("q3config.cfg")
    assert 'seta cg_fov "110"' in written["text"]
    # the box's previous file is archived against the deployment
    con = pp.connect()
    row = con.execute("SELECT backup FROM deployments WHERE action='apply' "
                      "ORDER BY id DESC LIMIT 1").fetchone()
    assert 'cg_fov "90"' in row["backup"]


def test_path_override_for_dual_boot_box(tmp_path, capsys, monkeypatch):
    """.124-style boxes keep games on D:, so --path must beat the gamedef."""
    pp = _pp(tmp_path)
    pp.main(["create", "p"])
    pp.main(["game-set", "p", "quake3", "cg_fov=110"])
    capsys.readouterr()
    seen = {}
    monkeypatch.setattr(pp, "agent_read_file",
                        lambda h, path: seen.setdefault("path", path) and None)
    monkeypatch.setattr(pp, "agent_write_file",
                        lambda h, path, text: seen.update(written=path))
    pp.main(["apply", "p", "quake3", "--host", "192.168.1.124",
             "--path", r"D:\Quake3\baseq3\q3config.cfg"])
    assert seen["written"] == r"D:\Quake3\baseq3\q3config.cfg"


def test_gamedef_can_be_added_and_overridden(tmp_path, capsys):
    pp = _pp(tmp_path)
    pp.main(["gamedef", "thespecialists", "--path", r"C:\HL\ts\config.cfg",
             "--format", "quake", "--cmd", ""])
    capsys.readouterr()
    pp.main(["games"])
    out = capsys.readouterr().out
    assert "thespecialists" in out and r"C:\HL\ts\config.cfg" in out
    # seeded defaults are still there
    assert "quake3" in out and "ut99" in out


def test_history_filters_by_host(tmp_path, capsys, monkeypatch):
    pp = _pp(tmp_path)
    pp.main(["create", "p"])
    monkeypatch.setattr(pp, "agent_read_file",
                        lambda h, path: 'seta cg_fov "110"\r\n')
    pp.main(["capture", "p", "quake3", "--host", "192.168.1.133"])
    pp.main(["capture", "p", "quake3", "--host", "192.168.1.185"])
    capsys.readouterr()
    pp.main(["history", "--host", "192.168.1.133"])
    out = capsys.readouterr().out
    assert "192.168.1.133" in out and "192.168.1.185" not in out
