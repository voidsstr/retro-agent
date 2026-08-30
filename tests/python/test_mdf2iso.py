"""`scripts/fleet/mdf2iso.py` - raw-CD geometry detection.

THE INVARIANT THIS PROTECTS. A Daemon Tools `.mdf` has **2448-byte sectors**
(2352 raw + 96 subchannel), so ISO sector 16 - where `CD001` lives - is at byte
`16*2448+16 = 39184`, **not** at 32768. On 2026-08-30, staging C&C Generals, a
`dd` at 32768 read zeros from all four discs and the images looked empty or
encrypted. They were neither. The test asserts detection lands on the right
geometry for every layout the fleet has met, and - the part that actually
catches a regression - that the payload extracted with the detected stride is
the payload that was written, byte for byte.
"""
import importlib.util
import io
import os
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPT = os.path.join(_HERE, "..", "..", "scripts", "fleet", "mdf2iso.py")


def _load():
    spec = importlib.util.spec_from_file_location("mdf2iso", _SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


mdf2iso = _load()


def _pvd(label: str) -> bytes:
    """A minimal ISO 9660 Primary Volume Descriptor payload (2048 bytes)."""
    b = bytearray(2048)
    b[0] = 1                       # descriptor type: primary
    b[1:6] = b"CD001"
    b[6] = 1                       # version
    b[40:40 + 32] = label.ljust(32).encode("latin-1")
    return bytes(b)


def _image(sector: int, off: int, payloads) -> bytes:
    """Wrap 2048-byte payloads into `sector`-byte sectors with the data at `off`.

    The filler is deliberately NOT zero: a stride bug that reads from the wrong
    place inside the sector would still produce plausible-looking zeros, and the
    whole point is to catch that.
    """
    out = bytearray()
    for i, p in enumerate(payloads):
        s = bytearray(((i + 7) * 37) % 251 or 1 for _ in range(sector))
        s[off:off + 2048] = p
        out += s
    return bytes(out)


CASES = [
    (2448, 16, "daemon tools mdf"),
    (2352, 16, "raw mode1 bin"),
    (2336, 8, "mode2 form1"),
    (2048, 0, "plain iso"),
]


@pytest.mark.parametrize("sector,off,name", CASES)
def test_detect_geometry(sector, off, name):
    payloads = [bytes([i % 251]) * 2048 for i in range(16)] + [_pvd("TESTVOL")]
    img = _image(sector, off, payloads)
    fh = io.BytesIO(img)
    assert mdf2iso.detect(fh) == (sector, off), name
    assert mdf2iso.volume_label(fh, sector, off) == "TESTVOL"


@pytest.mark.parametrize("sector,off,name", CASES)
def test_convert_round_trips_the_payload(tmp_path, sector, off, name):
    payloads = [bytes([(i * 3 + 1) % 251]) * 2048 for i in range(16)]
    payloads.append(_pvd("TESTVOL"))
    payloads.append(b"tail" * 512)
    src = tmp_path / "in.img"
    dst = tmp_path / "out.iso"
    src.write_bytes(_image(sector, off, payloads))

    with open(src, "rb") as fh:
        det = mdf2iso.detect(fh)
    assert det == (sector, off)

    n = mdf2iso.convert(str(src), str(dst), sector, off)
    assert n == len(payloads)
    assert dst.read_bytes() == b"".join(payloads), name


def test_pvd_is_not_at_32768_on_a_2448_image():
    """The specific mistake that cost the time, pinned as an assertion.

    32768 is where a plain ISO's PVD sits and where everyone looks first. On a
    2448-byte-sector image it is not the PVD - so a tool that hard-codes 32768
    reads the wrong bytes and calls the image empty.
    """
    payloads = [b"\x00" * 2048 for _ in range(16)] + [_pvd("GENERALS1")]
    img = _image(2448, 16, payloads)
    assert img[32768:32773] != b"CD001"
    assert img[16 * 2448 + 16 + 1:16 * 2448 + 16 + 6] == b"CD001"
    assert mdf2iso.detect(io.BytesIO(img)) == (2448, 16)


def test_no_volume_descriptor_raises_rather_than_guessing():
    """An unrecognisable image must fail loudly, never default to 2048.

    Guessing 2048 writes an ISO that opens nowhere, and the failure then surfaces
    minutes later as "7z says it is not an archive" - a symptom that points at
    the wrong thing.
    """
    with pytest.raises(ValueError) as e:
        mdf2iso.detect(io.BytesIO(b"\x9d" * (2448 * 20)))
    assert "volume descriptor" in str(e.value)


def test_cli_probe_reports_geometry(tmp_path, capsys):
    payloads = [b"\x00" * 2048 for _ in range(16)] + [_pvd("GENERALS1")]
    p = tmp_path / "g.mdf"
    p.write_bytes(_image(2448, 16, payloads))
    assert mdf2iso.main([str(p), "--probe"]) == 0
    out = capsys.readouterr().out
    assert "2448-byte sectors" in out
    assert "GENERALS1" in out
