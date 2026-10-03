# The A/B runner that produced join_A/join_B (2026-10-02), kept as the record of
# the procedure: upload a pilot to .123, verify it by md5, join the dev host's
# descent3-server with the Join launcher's own arguments at the given mode,
# capture at 25/45/65 s, take the non-black bounding box, PROCKILL (Direct3D).
#   python3 d3join_ab.py <outdir> <pilot file> <width> <height>
import asyncio, json, hashlib, sys, time
from io import BytesIO
from pathlib import Path
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[5]))
from client.retro_protocol import RetroConnection
H = '192.168.1.123'
OUT = Path(sys.argv[1]); OUT.mkdir(parents=True, exist_ok=True)
PILOT = Path(sys.argv[2]); W, Hh = int(sys.argv[3]), int(sys.argv[4])
async def c(cmd, t=60, raw=False, payload=None):
    x = RetroConnection(H, 9898); await x.connect('retro-agent-secret', timeout=10)
    try:
        r = (await (x.send_command(cmd, timeout=t, binary_payload=payload) if payload is not None else x.send_command(cmd, timeout=t)))[1]
        return r if raw else r.decode('latin-1', 'replace')
    finally: await x.close()
async def pids():
    pl = json.loads(await c('PROCLIST')); procs = pl if isinstance(pl, list) else pl.get('processes', [])
    return [p.get('pid') for p in procs if (p.get('name') or '').lower() == 'main.exe']
def bbox(im):
    return im.convert('L').point(lambda v: 255 if v > 12 else 0).getbbox()
async def m():
    rec = {'pilot': PILOT.name, 'pilot_md5': hashlib.md5(PILOT.read_bytes()).hexdigest(), 'mode': f'{W}x{Hh}', 'started': time.strftime('%Y-%m-%d %H:%M:%S')}
    assert not await pids(), 'main.exe already running'
    await c(r'UPLOAD C:\Games\Descent3\sdf.plt', payload=PILOT.read_bytes())
    back = await c(r'DOWNLOAD C:\Games\Descent3\sdf.plt', raw=True)
    assert hashlib.md5(back).hexdigest() == rec['pilot_md5'], 'pilot upload did not land'
    bat = ('@echo off\r\ncd /d "C:\\Games\\Descent3"\r\n'
           f'start "" main.exe -launched -nointro -pilot SDF -directip +connect 192.168.1.132 -Width {W} -Height {Hh}\r\n')
    await c(r'UPLOAD C:\WINDOWS\TEMP\d3join.bat', payload=bat.encode()); await c(r'EXEC cmd /c C:\WINDOWS\TEMP\d3join.bat'); await c(r'DELETE C:\WINDOWS\TEMP\d3join.bat')
    shots = {}
    for t in (25, 45, 65):
        await asyncio.sleep(20)
        im = Image.open(BytesIO(await c('SCREENSHOT 0', t=90, raw=True))).convert('RGB')
        im.save(OUT / f'join_{t}s.png'); shots[t] = {'size': im.size, 'nonblack_bbox': bbox(im)}
        print(t, 's', shots[t], 'fg', (json.loads(await c('WINLIST')).get('foreground') or {}).get('title'), flush=True)
    rec['shots'] = {str(k): {'size': list(v['size']), 'nonblack_bbox': list(v['nonblack_bbox']) if v['nonblack_bbox'] else None} for k, v in shots.items()}
    for pid in await pids():
        rec.setdefault('prockill', []).append([pid, await c(f'PROCKILL {pid}')])
    await asyncio.sleep(4)
    rec['main_exe_after'] = await pids()
    rec['desktop_after'] = json.loads(await c('GAMERES')).get('desktop')
    (OUT / 'run.json').write_text(json.dumps(rec, indent=2) + '\n'); print(json.dumps(rec)[:600])
asyncio.run(m())
