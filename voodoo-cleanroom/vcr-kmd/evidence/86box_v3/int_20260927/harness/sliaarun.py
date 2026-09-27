#!/usr/bin/env python3
"""sliaarun.py <outdir> <label> <vcrctl sliaa args...> - one `vcrctl sliaa`
on the 86Box guest (127.0.0.1:19920 ONLY): info before/after (desktop mode,
the display driver's exclusive owner), the tool's JSON, and the recorder
entries after it - SLI register writes (SLI_STEP 100-899), refusals (901),
HWC_SLIAA / SLI_DONE, HWC_EXCLUSIVE and mode sets."""
import asyncio, json, sys
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

def last_json(t):
    for ln in reversed(t.strip().splitlines()):
        if ln.startswith('{'):
            try:
                return json.loads(ln)
            except ValueError:
                return None
    return None

def info():
    return last_json(asyncio.run(ex(r'EXEC C:\vcr\vcrctl.exe info'))) or {}

KEYS = ('cur', 'cur_mode', 'dd_mode', 'exclusive_pid', 'hwc_requests', 'sli_active', 'sli_chips',
        'sli_result', 'glide_chips', 'log_next_seq')
out, label, rest = Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
out.mkdir(parents=True, exist_ok=True)
i0 = info()
seq0 = i0.get('log_next_seq', 0)
cmd = r'EXEC C:\vcr\vcrctl.exe sliaa ' + ' '.join(rest)
t = asyncio.run(ex(cmd, 120))
res = last_json(t)
i1 = info()
disp = asyncio.run(ex('DISPLAYCFG get')).strip()
logtxt = asyncio.run(ex(rf'EXEC C:\vcr\vcrctl.exe log {seq0}'))
(out / f'{label}.txt').write_text(f'# {cmd}\n{t}\n# info before: {json.dumps({k: i0.get(k) for k in KEYS})}\n'
                                  f'# info after:  {json.dumps({k: i1.get(k) for k in KEYS})}\n# DISPLAYCFG get after: {disp}\n')
(out / f'{label}.vcrlog.tsv').write_text(logtxt)
hdr, ents = vcrlog.parse_tsv(logtxt)
ev = vcrlog.load_events()
ents = [e for e in ents if e['seq'] > seq0]
name = lambda e: ev.get(e['code'], (str(e['code']),))[0]
writes = [vcrlog.format_entry(e, ev) for e in ents if name(e) == 'SLI_STEP' and 100 <= e['a'] < 900]
refused = [vcrlog.format_entry(e, ev) for e in ents if name(e) == 'SLI_STEP' and e['a'] >= 900]
sliaa = [vcrlog.format_entry(e, ev) for e in ents if name(e) in ('HWC_SLIAA', 'SLI_DONE', 'SLI_POKE_REFUSED')]
excl = [vcrlog.format_entry(e, ev) for e in ents if name(e) == 'HWC_EXCLUSIVE']
modes = [vcrlog.format_entry(e, ev) for e in ents if name(e) in ('MODESET_BEGIN',) and 'mode ' in e['msg']]
warn = [vcrlog.format_entry(e, ev) for e in ents if e['level'] <= 1]
summ = {'label': label, 'args': ' '.join(rest), 'tool': res, 'info_before': {k: i0.get(k) for k in KEYS},
        'info_after': {k: i1.get(k) for k in KEYS}, 'displaycfg_after': disp, 'seq0': seq0,
        'sli_register_writes': writes, 'refused_steps': refused, 'sliaa_events': sliaa,
        'exclusive': excl, 'modesets': modes, 'warn_err': warn}
with open(out / 'sliaa_summary.jsonl', 'a') as f:
    f.write(json.dumps(summ) + '\n')
print(label, '|', ' '.join(rest))
print('   tool:', json.dumps(res)[:420])
print('   after: cur', i1.get('cur'), 'exclusive_pid', i1.get('exclusive_pid'), 'sli_result', i1.get('sli_result'),
      'sli_active', i1.get('sli_active'), '| DISPLAYCFG', disp)
print('   SLI register writes:', len(writes))
for x in writes[:5] + refused + sliaa + excl + modes:
    print('   ', x[:230])
