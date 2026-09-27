#!/usr/bin/env python3
"""d3drun.py <outdir> <label> <d3dprobe_run args...> - one d3dprobe run on the
86Box guest (127.0.0.1:19920 ONLY) plus the recorder entries it produced;
names the refusing layer: 513/12 (DD_D3D target refused), 511/11
(CanCreateSurface Z refused), or runtime-only (neither logged)."""
import asyncio, json, subprocess, sys
from pathlib import Path
KMD = Path('/home/voidsstr/development/retro-agent/.claude/worktrees/vk-int/voodoo-cleanroom/vcr-kmd')
sys.path.insert(0, str(KMD.parents[1]))
sys.path.insert(0, str(KMD / 'tools'))
from client.retro_protocol import RetroConnection
import vcrlog

async def ex(cmd, t=90):
    c = RetroConnection('127.0.0.1', 19920)
    await c.connect('retro-agent-secret', timeout=20)
    try:
        st, d = await c.send_command(cmd, timeout=t)
    finally:
        await c.close()
    return d.decode('latin1', 'replace')

def info():
    t = asyncio.run(ex(r'EXEC C:\vcr\vcrctl.exe info'))
    for ln in t.splitlines():
        if ln.startswith('{'):
            return json.loads(ln)
    return {}

out, label, rest = Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
tool = 'd3dprobe_run.py'
if rest and rest[0] == '--tool=ddlab':
    tool, rest = 'ddlab_run.py', rest[1:]
out.mkdir(parents=True, exist_ok=True)
i0 = info()
seq0 = i0.get('log_next_seq', 0)
r = subprocess.run([sys.executable, str(KMD / 'tools' / tool), '127.0.0.1', '--port', '19920'] + rest + ['-v'],
                   capture_output=True, text=True, timeout=900)
(out / f'{label}.txt').write_text(f'# {tool} {" ".join(rest)} -v  (rc {r.returncode}; desktop {i0.get("cur")})\n' + r.stdout + r.stderr)
res = None
for ln in r.stdout.splitlines():
    if ln.startswith('{'):
        try:
            res = json.loads(ln)
        except ValueError:
            pass
i1 = info()
logtxt = asyncio.run(ex(rf'EXEC C:\vcr\vcrctl.exe log {max(seq0 - 1, 0)}'))
(out / f'{label}.vcrlog.tsv').write_text(logtxt)
hdr, ents = vcrlog.parse_tsv(logtxt)
ev = vcrlog.load_events()
ents = [e for e in ents if e['seq'] > seq0]   # log_next_seq is the LAST seq written
refuse = [vcrlog.format_entry(e, ev) for e in ents
          if (e['code'] == 513 and e['a'] in (12, 15, 17)) or (e['code'] == 511 and e['a'] == 11)]
ctx = [vcrlog.format_entry(e, ev) for e in ents if e['code'] == 513 and e['a'] in (1, 2, 16)]
warn = [vcrlog.format_entry(e, ev) for e in ents if e['level'] <= 1]
layer = ('513/12' if any(' DD_D3D ' in x and x.split()[5] == '0000000c' for x in refuse) else '') 
layers = sorted({('513/%d' % int(x.split()[5], 16)) if ' DD_D3D ' in x else '511/11' for x in refuse})
summ = {'label': label, 'args': ' '.join(rest), 'rc': r.returncode, 'desktop_before': i0.get('cur'),
        'desktop_after': i1.get('cur'), 'seq0': seq0,
        'result': ({k: v for k, v in res.items() if k != 'checks'} if isinstance(res, dict) else res),
        'refusing_layer': layers or ['runtime-only (no 513/12, 513/15, 513/17 or 511/11 logged)'],
        'refusals': refuse, 'd3d_ctx': ctx, 'warn_err': warn}
with open(out / 'd3d_summary.jsonl', 'a') as f:
    f.write(json.dumps(summ) + '\n')
print(label, 'rc', r.returncode, 'desktop', i0.get('cur'), '->', i1.get('cur'))
print('   result:', json.dumps(summ['result'])[:400])
print('   layer:', summ['refusing_layer'])
for x in refuse + ctx + warn:
    print('   ', x[:220])
