"""The gate publisher writes from THIS host, not through a fleet box (2026-09-29).

WHY. publish_all.py wrote every verdict file by UPLOADing through the first
fleet box that could see Z:, and checked and counted with cmd.exe EXEC'd on
that box. .124 was first on the list, so a publish for .243 ran cmd.exe on .124
in the middle of another session's V5 6000 benchmark campaign - where a stray
process is a skewed number. scripts/fleet/sharewrite.py now writes the share
headless (smbclient + the vaulted NAS credentials, md5 read back through /mnt),
so the publisher uses it by default and counts rows through /mnt; the box route
is the fallback, and .124 is its last candidate.
"""
import asyncio
import importlib.util
import os
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "scripts"))
sys.path.insert(0, REPO)
spec = importlib.util.spec_from_file_location(
    "publish_all", os.path.join(REPO, "scripts", "gamegate", "publish_all.py"))
pa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pa)


def test_rows_are_counted_the_way_find_v_c_counts_them():
    # `find /v /c "#"`: every line WITHOUT a '#', blank lines included.
    data = b"# gate verdicts\r\n# profile x\r\nrun\tQuake1\t-\r\nno\tHalo\t-\r\n\r\n"
    assert pa.count_rows(data) == 3
    assert pa.count_rows(b"") == 0
    assert pa.count_rows(b"run\tA\t-\r\nrun\tB\t-") == 2    # no final newline


def test_the_host_writer_reads_the_rows_back_through_mnt(tmp_path, monkeypatch):
    monkeypatch.setattr(pa, "MNT_DIR", str(tmp_path))
    (tmp_path / "abc.txt").write_bytes(b"# h\r\nrun\tA\t-\r\nno\tB\t-\r\n")
    w = pa.HostWriter()
    assert asyncio.run(w.rows("abc.txt")) == 2
    assert asyncio.run(w.rows("missing.txt")) == -1, "an absent file is not 0 rows"


def test_by_default_no_fleet_box_is_touched(monkeypatch):
    monkeypatch.setattr(pa, "WRITER", "")
    monkeypatch.setattr(pa.HostWriter, "available", staticmethod(lambda: True))

    async def boom():
        raise AssertionError("a fleet box was picked as the writer")
    monkeypatch.setattr(pa, "_pick_writer", boom)
    w = asyncio.run(pa._open_writer())
    assert isinstance(w, pa.HostWriter)


class _Picked(Exception):
    pass


def test_an_ip_override_still_takes_the_box_route(monkeypatch):
    monkeypatch.setattr(pa, "WRITER", "192.168.1.123")
    monkeypatch.setattr(pa.HostWriter, "available", staticmethod(lambda: True))

    async def picked():
        raise _Picked()
    monkeypatch.setattr(pa, "_pick_writer", picked)
    with pytest.raises(_Picked):
        asyncio.run(pa._open_writer())


def test_without_the_host_route_it_falls_back_to_a_box(monkeypatch):
    monkeypatch.setattr(pa, "WRITER", "")
    monkeypatch.setattr(pa.HostWriter, "available", staticmethod(lambda: False))

    async def picked():
        raise _Picked()
    monkeypatch.setattr(pa, "_pick_writer", picked)
    with pytest.raises(_Picked):
        asyncio.run(pa._open_writer())


def test_the_benchmark_box_is_the_last_box_route_candidate():
    assert pa.WRITER_CANDIDATES[-1] == "192.168.1.124"
    assert "192.168.1.243" not in pa.WRITER_CANDIDATES


def test_every_write_and_count_goes_through_the_writer():
    src = open(os.path.join(REPO, "scripts", "gamegate", "publish_all.py")).read()
    body = src[src.index("async def main_async"):]
    assert "writer.write(" in body and "writer.rows(" in body
    assert "_write(conn" not in body and "_rows_on_share(conn" not in body
