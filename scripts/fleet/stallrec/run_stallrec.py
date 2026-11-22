#!/usr/bin/env python3
"""Upload stallrec.exe to a fleet box, run it detached, fetch the log.

Keeps the whole measurement in one repeatable command, because the thing this
tool exists to catch is periodic - and a measurement you cannot repeat
identically cannot show a fix.
"""
import argparse, asyncio, os, pathlib, sys, time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_SECRET", "retro-agent-secret")
REMOTE_EXE = r"C:\RETRO_AGENT\stallrec.exe"


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--seconds", type=int, default=150)
    ap.add_argument("--threshold", type=float, default=20.0)
    ap.add_argument("--label", default="run")
    ap.add_argument("--out", default=None, help="where to save the log locally")
    ap.add_argument("--no-upload", action="store_true")
    a = ap.parse_args()

    remote_log = rf"C:\stalls-{a.label}.txt"
    local = pathlib.Path(a.out or (HERE / f"results-{a.host}-{a.label}.txt"))

    c = RetroConnection(a.host, 9898)
    await c.connect(SECRET, timeout=15.0)
    try:
        if not a.no_upload:
            exe = (HERE / "stallrec.exe").read_bytes()
            await c.send_command(f"UPLOAD {REMOTE_EXE}", binary_payload=exe)
            print(f"uploaded stallrec.exe ({len(exe)} bytes)")

        cmd = (f'EXEC cmd /c start /b "" {REMOTE_EXE} '
               f"{a.seconds} {a.threshold:g} {remote_log}")
        await c.send_command(cmd)
        print(f"recording {a.seconds}s (threshold {a.threshold:g} ms) -> {remote_log}")
    finally:
        await c.close()

    # Poll from a fresh connection so the box is left alone while it records.
    deadline = time.time() + a.seconds + 30
    while time.time() < deadline:
        await asyncio.sleep(10)
        c = RetroConnection(a.host, 9898)
        try:
            await c.connect(SECRET, timeout=15.0)
            _, data = await c.send_command(f"EXEC cmd /c find /c \"finished\" {remote_log}")
            if b"finished" in data or b": 1" in data:
                break
        except Exception:
            pass
        finally:
            await c.close()

    c = RetroConnection(a.host, 9898)
    await c.connect(SECRET, timeout=15.0)
    try:
        _, blob = await c.send_command(f"DOWNLOAD {remote_log}")
    finally:
        await c.close()
    local.write_bytes(blob)
    text = blob.decode("ascii", "replace")
    print(text)
    print(f"saved {local}")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
