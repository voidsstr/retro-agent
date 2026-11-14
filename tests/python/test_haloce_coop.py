"""Halo CE co-op (2026-11-13): the per-box key file carries BOTH Halo and Halo
CE stanzas, and the Halo GameSpy2 probe parses a real reply shape."""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts", "game-servers"))


def test_make_dpid_reg_writes_halo_and_halo_ce():
    sys.path.insert(0, os.path.join(ROOT, "scripts", "halo"))
    import make_dpid
    k = make_dpid.ALPHABET[0] * 25          # shape only; not a real key
    with tempfile.NamedTemporaryFile("w", delete=False) as f:
        f.write(k)
    try:
        out = subprocess.run([sys.executable, os.path.join(ROOT, "scripts/halo/make_dpid.py"),
                              "--key-file", f.name, "--reg"],
                             capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(f.name)
    assert "Microsoft Games\\Halo]" in out
    assert "Microsoft Games\\Halo CE]" in out
    assert out.count('"DigitalProductID"=hex:') == 2


def test_probe_halo_parses_gamespy2(monkeypatch):
    import gameservers
    reply = (b"\x00RETR" + b"\x00".join([
        b"hostname", b"Retro Fleet Co-op", b"gamever", b"01.00.10.0621",
        b"maxplayers", b"16", b"mapname", b"a10_coop_evolved",
        b"numplayers", b"3", b"gamevariant", b"coop", b"", b""]))
    monkeypatch.setattr(gameservers, "_ask", lambda *a, **k: (reply, 1.0))
    r = gameservers.probe_halo(2302)
    assert r["name"] == "Retro Fleet Co-op" and r["map"] == "a10_coop_evolved"
    assert r["players"] == 3 and r["humans"] == 3 and r["bots"] == 0
    assert gameservers.PROBES["halo"] is gameservers.probe_halo


def test_probe_halo_rejects_garbage(monkeypatch):
    import gameservers
    monkeypatch.setattr(gameservers, "_ask", lambda *a, **k: (b"\xff\xff", 1.0))
    assert gameservers.probe_halo(2302) is None
