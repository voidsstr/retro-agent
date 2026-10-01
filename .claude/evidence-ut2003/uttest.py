#!/usr/bin/env python3
"""Launch UT2003/UT2004 through its staged Play launcher on a box, prove it is
fullscreen and takes keyboard input, then close it.  (ad hoc, 2026-09-29)

    uttest.py <ip> UT2003|UT2004 [wait_s]
Evidence: .claude/evidence-ut2003/<ip>/<title>-*.png + result.json
"""
import asyncio, io, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
from PIL import Image, ImageChops, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))


async def shot(c, path):
    b = await c.command_binary('SCREENSHOT 0', timeout=90)
    im = Image.open(io.BytesIO(b)).convert('RGB')
    im.save(path)
    return im


async def main(ip, title, wait_s):
    out = os.path.join(HERE, ip); os.makedirs(out, exist_ok=True)
    exe = title + '.exe'
    res = {'ip': ip, 'title': title}
    c = RetroConnection(ip, 9898); await c.connect('retro-agent-secret', timeout=10)
    g = json.loads(await c.command_text('GAMESYNC STATUS', timeout=60))
    dest = g.get('dest') or 'C:\\Games'
    tdir = '%s\\%s' % (dest, title)
    bat = (await c.command_binary('DOWNLOAD %s\\Play %s.bat' % (tdir, title), timeout=60)).decode('latin1')
    res['launcher_has_reseed'] = 'UE_USERINI' in bat
    try:
        ui = (await c.command_binary('DOWNLOAD %s\\System\\User.ini' % tdir, timeout=60)).decode('latin1')
        res['userini_before'] = {'size': len(ui), 'has_input': '[engine.input]' in ui.lower()}
    except Exception as e:
        res['userini_before'] = 'absent (%s)' % str(e)[:60]
    await c.command_text('EXEC taskkill /f /im %s' % exe, timeout=50)
    res['launch'] = await c.command_text(
        'EXEC cmd /c start "" /D "%s" "Play %s.bat"' % (tdir, title), timeout=50)
    await c.close()

    await asyncio.sleep(wait_s)
    c = RetroConnection(ip, 9898); await c.connect('retro-agent-secret', timeout=10)
    w = json.loads(await c.command_text('WINLIST', timeout=60))
    res['foreground'] = w['foreground']
    res['windows'] = [(x['title'], x['class'], x['rect']) for x in w['windows']][:8]
    res['displaycfg'] = (await c.command_text('DISPLAYCFG get', timeout=60))[:400]
    a = await shot(c, os.path.join(out, '%s-1-running.png' % title))
    res['screen'] = a.size
    # Keyboard: ESC toggles the in-game menu (or leaves the intro).
    await c.command_text('UIKEY ESCAPE', timeout=30); await asyncio.sleep(4)
    b = await shot(c, os.path.join(out, '%s-2-after-esc.png' % title))
    diff = ImageChops.difference(a, b)
    res['esc_changed_pct'] = round(100.0 * sum(1 for p in diff.getdata() if sum(p) > 30) / (a.size[0] * a.size[1]), 2)
    await c.command_text('UIKEY ESCAPE', timeout=30); await asyncio.sleep(3)
    await shot(c, os.path.join(out, '%s-3-after-esc2.png' % title))
    try:
        ui = (await c.command_binary('DOWNLOAD %s\\System\\User.ini' % tdir, timeout=60)).decode('latin1')
        res['userini_after'] = {'size': len(ui), 'has_input': '[engine.input]' in ui.lower()}
    except Exception as e:
        res['userini_after'] = 'absent (%s)' % str(e)[:60]
    await c.command_text('EXEC taskkill /im %s' % exe, timeout=50)
    await asyncio.sleep(5)
    await c.command_text('EXEC taskkill /f /im %s' % exe, timeout=50)
    await c.close()
    json.dump(res, open(os.path.join(out, '%s-result.json' % title), 'w'), indent=1, default=str)
    print(json.dumps(res, indent=1, default=str))


if __name__ == '__main__':
    asyncio.run(main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 45))
