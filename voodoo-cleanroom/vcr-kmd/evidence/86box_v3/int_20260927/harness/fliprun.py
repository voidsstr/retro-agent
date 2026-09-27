#!/usr/bin/env python3
"""fliprun.py <outdir> <label> <ddlab_run args...> - one ddlab run on the 86Box
guest (127.0.0.1:19920 ONLY) plus the recorder entries it produced: saves
<label>.txt (ddlab_run output), <label>.vcrlog.tsv (log after the pre-run
seq), and appends a summary line to <outdir>/flip_summary.jsonl."""
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
flush = False
if rest and rest[0] == '--flush':
    # a same-mode session's counters are logged only when the desktop PDEV's
    # mode leaves the screen: one paced temporary switch (reverted at exit)
    flush, rest = True, rest[1:]
out.mkdir(parents=True, exist_ok=True)
seq0 = info().get('log_next_seq', 0)
r = subprocess.run([sys.executable, str(KMD / 'tools' / 'ddlab_run.py'), '127.0.0.1', '--port', '19920'] + rest,
                   capture_output=True, text=True, timeout=900)
(out / f'{label}.txt').write_text(f'# ddlab_run.py {" ".join(rest)}  (rc {r.returncode})\n' + r.stdout + r.stderr)
res = None
for ln in r.stdout.splitlines():
    if ln.startswith('{'):
        res = json.loads(ln)
flushed = None
if flush:
    flushed = asyncio.run(ex(r'EXEC C:\vcr\vcrctl.exe setmode 640 480 16', 120)).strip().splitlines()[-1:]
logtxt = asyncio.run(ex(rf'EXEC C:\vcr\vcrctl.exe log {max(seq0 - 1, 0)}'))
(out / f'{label}.vcrlog.tsv').write_text(logtxt)
hdr, ents = vcrlog.parse_tsv(logtxt)
ev = vcrlog.load_events()
ents = [e for e in ents if e['seq'] > seq0]   # log_next_seq is the LAST seq written
stats = []
modesets = []
for e in ents:
    if e['code'] == 511 and e['a'] in (12, 13):
        stats.append({'seq': e['seq'], 'a': e['a'], 'msg': e['msg']})
    if ev.get(e['code'], ('',))[0] == 'MODESET_BEGIN' and 'mode ' in e['msg']:
        modesets.append(e['msg'])
    if ev.get(e['code'], ('',))[0] == 'MODESET_PLL':
        modesets.append(e['msg'])
warn = [vcrlog.format_entry(e, ev) for e in ents if e['level'] <= 1]
summ = {'label': label, 'args': ' '.join(rest), 'rc': r.returncode, 'seq0': seq0,
        'result': res, 'flush': flushed, 'flip_stats': stats, 'modesets': modesets, 'warn_err': warn}
with open(out / 'flip_summary.jsonl', 'a') as f:
    f.write(json.dumps(summ) + '\n')
keys = ('flips_s', 'vblank_hz', 'mismatch', 'scanline', 'work_us', 'first_frame_ms', 'max_frame_ms', 'slow_frames', 'min_frame_ms', 'fast_frames', 'flips_s_first_last', 'error')
print(label, 'rc', r.returncode, {k: (res or {}).get(k) for k in keys})
for s in stats:
    print('   ', s['msg'])
for m in modesets:
    print('    modeset:', m)
for w in warn:
    print('    WARN/ERR:', w)
