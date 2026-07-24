#!/usr/bin/env python3
r"""
recipe_specialists.py - worked example: install The Specialists 3.0 (a Clickteam
Half-Life mod installer, no silent switch) end-to-end on one box, real-time.

    python3 recipe_specialists.py <ip>

Copy this shape for other button-clicking installers: stage -> launch -> walk with
CLICKSHOT -> poll-until-stable -> relocate_verified. The click sequence here is the
Clickteam flow (Welcome->Next, Dir->Next, "create dir?"->Yes, Confirm->Start,
End->Next, nag->Exit); button offsets are relative to the installer window rect so
they work at any resolution. For a different installer, read each returned delta
frame (ui.image()) and click what you see instead of hard-coding offsets.

Notes baked in from the fleet:
  - installer default = %ProgramFiles%\The Specialists\ts  (D: on dual-boot boxes)
  - mod gamedir is `ts`; final home is <HL>\ts  (HL usually C:\Sierra\Half-Life)
  - same-volume -> move; cross-volume (D:->C:) -> tree_copy_via_batch (xcopy is
    broken on several boxes); verify file-count parity before deleting the source.
"""
import os, sys, asyncio, json

sys.path.insert(0, os.path.dirname(__file__))
from fastui import FastUI
import install_lib as lib

SHARE = r'Z:\Games\Mods & Patches\Half-Life\The_Specialists_v3.0__ts-3-final.exe'
LOCAL = r'C:\ts3.exe'
KEY_FILES = ['liblist.gam', r'dlls\mp.dll', r'cl_dlls\client.dll']


def win_by(wins, cls=None, title_sub=None):
    for w in wins:
        if cls and w.get('class') != cls:
            continue
        if title_sub and title_sub not in w.get('title', ''):
            continue
        return w
    return None


async def wait_win(ui, cls=None, title_sub=None, secs=30):
    for _ in range(secs // 2):
        w = win_by(await ui.winlist(), cls, title_sub)
        if w:
            return w
        await asyncio.sleep(2)
    return None


async def run(ip):
    ui = FastUI(ip)
    await ui.connect()
    try:
        # 0. locate HL + program files
        pf = await lib.env(ui.c, 'ProgramFiles')
        hl = 'C:\\Sierra\\Half-Life'  # adjust from HKLM\SOFTWARE\Valve\Half-Life if needed
        stub = pf + r'\The Specialists'
        ts_src = stub + r'\ts'
        ts_dst = hl + r'\ts'
        cross = ts_src[:2].upper() != ts_dst[:2].upper()

        # 1. stage
        print('stage:', await lib.stage_from_share(ui.c, SHARE, LOCAL))

        # 2. launch + baseline
        await ui.c.send_command(f'LAUNCH {LOCAL}', timeout=20)
        w = await wait_win(ui, cls='InstItClass', secs=30)
        if not w:
            print('installer window never appeared'); return
        await ui.baseline()

        # helper: click a button at an offset within the InstItClass window
        async def click_main(dx, dy):
            cur = win_by(await ui.winlist(), cls='InstItClass')
            r = cur['rect']
            await ui.clickshot(r['left'] + dx, r['bottom'] + dy, settle_ms=120)

        # 3. walk: Welcome->Next, Dir->Next
        await click_main(361, -27)      # Next (welcome)
        await asyncio.sleep(1)
        await click_main(361, -27)      # Next (directory) -> create-dir prompt
        dlg = await wait_win(ui, cls='#32770', title_sub='Install Program', secs=12)
        if dlg:
            r = dlg['rect']
            await ui.clickshot(r['left'] + 162, r['bottom'] - 26, settle_ms=120)  # Yes
        await click_main(361, -27)      # Start
        # 4. wait for the copy to finish
        await lib.poll_until_stable(ui.c, ts_src, marker_file=ts_src + r'\liblist.gam',
                                    min_bytes=200_000_000)
        # close installer: End->Next, nag->Exit
        await click_main(361, -27)
        await asyncio.sleep(2)
        await click_main(444, -27)
        await ui.c.send_command('EXEC cmd /c taskkill /f /im ts3.exe 2>nul', timeout=20)

        # 5. relocate + verify (never delete on failure)
        res = await lib.relocate_verified(ui.c, ts_src, ts_dst, KEY_FILES, cross_volume=cross)
        print('relocate:', json.dumps(res))
        if res['ok']:
            await ui.c.send_command(
                f'EXEC cmd /c rmdir /s /q "{stub}" 2>nul & del /f /q {LOCAL} 2>nul & echo cleaned',
                timeout=60)
            print(f'{ip}: The Specialists installed at {ts_dst}')
        else:
            print(f'{ip}: NOT verified - left source intact for retry')
    finally:
        await ui.close()


if __name__ == '__main__':
    asyncio.run(run(sys.argv[1] if len(sys.argv) > 1 else '192.168.1.133'))
