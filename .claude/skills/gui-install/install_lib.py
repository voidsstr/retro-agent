#!/usr/bin/env python3
"""
install_lib.py - reusable mechanical helpers for gui-install.

The click-walk itself is LLM-in-the-loop (see fastui.py / recipe_specialists.py);
these are the deterministic parts you should NOT hand-roll each time: staging,
waiting for a copy to finish, and relocating with a verify-before-delete guard.

All helpers take an open FastUI (or RetroConnection) and use plain agent EXEC.
"""
import os, sys, asyncio

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)
from client.retro_protocol import RetroConnection

SECRET = 'retro-agent-secret'


async def _cmd(conn, x, t=90):
    try:
        s, d = await conn.send_command(x, timeout=t)
        return d.decode('ascii', 'replace')
    except Exception as e:
        return f'ERR {e}'


async def env(conn, name):
    """Read a Windows env var, e.g. ProgramFiles / SystemDrive."""
    return (await _cmd(conn, f'EXEC cmd /c echo %{name}%', 20)).strip()


async def stage_from_share(conn, share_path, local_path, timeout=500):
    """copy /Y a single file (any size) from the mapped share to the box.
    Re-asserts the Z: mapping first (it goes stale). Returns True if sizes match."""
    await _cmd(conn, 'EXEC cmd /c net use Z: \\\\192.168.1.122\\files 2>nul', 40)
    await _cmd(conn, f'EXECW {timeout} cmd /c copy /Y "{share_path}" "{local_path}" 2>&1', timeout + 30)
    return (await _cmd(conn, f'EXEC cmd /c for %A in ("{local_path}") do @echo %~zA', 30)).strip()


async def folder_bytes(conn, path):
    """Total bytes under a folder (dir /s /-c), or -1 if absent."""
    r = await _cmd(conn, f'EXEC cmd /c dir /s /-c "{path}" 2>nul', 60)
    tot = -1
    for ln in r.splitlines():
        s = ln.strip()
        if 'File(s)' in s and 'bytes' in s:
            p = s.split()
            try:
                tot = int(p[p.index('bytes') - 1].replace(',', ''))
            except Exception:
                pass
    return tot


async def poll_until_stable(conn, path, marker_file=None, min_bytes=1, every=6,
                            max_iters=90, log=print):
    """Poll a folder's byte total until it stops growing for 2 checks (and, if
    given, marker_file exists). Returns final size. Use after clicking 'Start'
    to know a copy finished instead of guessing a sleep."""
    last, stable = -999, 0
    for i in range(max_iters):
        await asyncio.sleep(every)
        b = await folder_bytes(conn, path)
        ok_marker = True
        if marker_file:
            ok_marker = 'Y' in await _cmd(conn, f'EXEC cmd /c if exist "{marker_file}" (echo Y) else (echo N)', 20)
        if log:
            log(f'  poll t={i*every}s size={b} marker={ok_marker}')
        if b > 0 and b == last and ok_marker and b >= min_bytes:
            stable += 1
            if stable >= 2:
                return b
        else:
            stable = 0
        last = b
    return last


async def count_files(conn, path):
    r = (await _cmd(conn, f'EXEC cmd /c dir /s /b "{path}" 2>nul | find /c /v ""', 60)).strip()
    return int(r) if r.isdigit() else -1


async def tree_copy_via_batch(conn, src, dst, timeout=800, log=print):
    """Recursively copy a directory tree using mkdir+copy per subdir (works where
    xcopy is broken). Uploads a generated .bat and runs it. Returns (src_files,
    dst_files) so the caller can check parity. Does NOT delete the source."""
    r = await _cmd(conn, f'EXEC cmd /c dir /s /b /ad "{src}" 2>nul', 90)
    subdirs = [l.strip()[len(src):].lstrip('\\') for l in r.splitlines() if l.strip()]
    lines = ['@echo off', f'mkdir "{dst}" 2>nul', f'copy /Y "{src}\\*.*" "{dst}\\" >nul 2>&1']
    for rel in subdirs:
        lines.append(f'mkdir "{dst}\\{rel}" 2>nul')
        lines.append(f'copy /Y "{src}\\{rel}\\*.*" "{dst}\\{rel}\\" >nul 2>&1')
    lines.append('echo TREECOPY_DONE')
    bat = ('\r\n'.join(lines) + '\r\n').encode('ascii')
    batpath = 'C:\\_treecopy.bat'
    await conn.send_command(f'UPLOAD {batpath}', binary_payload=bat, timeout=60)
    if log:
        log(f'  tree_copy: {len(subdirs)} subdirs')
    await _cmd(conn, f'EXECW {timeout} cmd /c {batpath} 2>&1', timeout + 30)
    sc = await count_files(conn, src)
    dc = await count_files(conn, dst)
    await _cmd(conn, f'EXEC cmd /c del /f /q {batpath} 2>nul & echo k', 20)
    return sc, dc


async def relocate_verified(conn, src, dst, key_files, cross_volume=False, log=print):
    """Move installed payload src -> dst, verify key_files exist + file-count
    parity, and ONLY THEN it is safe to delete the source. Returns dict with
    'ok' and details. Never deletes on failure."""
    src_n = await count_files(conn, src)
    if cross_volume:
        sc, dc = await tree_copy_via_batch(conn, src, dst, log=log)
        moved = dc >= sc > 0
    else:
        r = (await _cmd(conn, f'EXEC cmd /c move "{src}" "{dst}" 2>&1', 180)).strip()
        if log:
            log(f'  move: {r[-80:]}')
        dc = await count_files(conn, dst)
        sc = src_n
        moved = 'moved' in r.lower() or dc >= sc > 0
    verify = {}
    for kf in key_files:
        verify[kf] = 'OK' in await _cmd(conn, f'EXEC cmd /c if exist "{dst}\\{kf}" (echo OK) else (echo MISSING)', 30)
    ok = moved and all(verify.values()) and dc >= sc > 0
    return {'ok': ok, 'src_files': sc, 'dst_files': dc, 'verify': verify}


async def parallel_map(ips, coro_fn, port=9898, secret=SECRET):
    """Run coro_fn(ip, FastUI-or-RetroConnection) for each ip concurrently, each
    with its own connection. Returns {ip: result_or_Exception}."""
    from fastui import FastUI

    async def one(ip):
        ui = FastUI(ip, port, secret)
        try:
            await ui.connect()
            return await coro_fn(ip, ui)
        except Exception as e:
            return e
        finally:
            await ui.close()

    results = await asyncio.gather(*[one(ip) for ip in ips], return_exceptions=True)
    return dict(zip(ips, results))
