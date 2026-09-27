import asyncio, json, sys
from pathlib import Path
KMD = Path('/home/voidsstr/development/retro-agent/.claude/worktrees/vk-int/voodoo-cleanroom/vcr-kmd')
sys.path.insert(0, str(KMD.parents[1])); sys.path.insert(0, str(KMD / 'tools'))
from client.retro_protocol import RetroConnection
import vcrlog
O = Path(sys.argv[1])
async def ex(cmd, t=90):
    c = RetroConnection('127.0.0.1', 19920); await c.connect('retro-agent-secret', timeout=20)
    try:
        st, d = await c.send_command(cmd, timeout=t)
    finally:
        await c.close()
    return d.decode('latin1', 'replace')
def lj(t):
    for ln in reversed(t.strip().splitlines()):
        if ln.startswith('{'):
            return json.loads(ln)
def info():
    j = lj(asyncio.run(ex(r'EXEC C:\vcr\vcrctl.exe info')))
    return {k: j.get(k) for k in ('cur', 'dd_mode', 'exclusive_pid', 'hwc_requests', 'sli_result', 'log_next_seq')}
lines = []
def say(*a):
    s = ' '.join(str(x) for x in a); print(s); lines.append(s)
i0 = info(); seq0 = i0['log_next_seq']
say('O1 info before:', json.dumps(i0))
say('O2', asyncio.run(ex(r'EXEC C:\vcr\scratch\stale_owner.exe set')).strip())
say('O2 info after the owner exited without a release:', json.dumps(info()))
say('O3', asyncio.run(ex(r'EXEC C:\vcr\scratch\stale_owner.exe rls')).strip())
say('O3 info after a NON-owner release:', json.dumps(info()))
say('O4 vcrctl sliaa off (separate process):', asyncio.run(ex(r'EXEC C:\vcr\vcrctl.exe sliaa off', 120)).strip())
i4 = info()
say('O4 info after off:', json.dumps(i4))
say('O4 DISPLAYCFG get:', asyncio.run(ex('DISPLAYCFG get')).strip())
logtxt = asyncio.run(ex(rf'EXEC C:\vcr\vcrctl.exe log {seq0}'))
(O / 'owner_sequence.vcrlog.tsv').write_text(logtxt)
hdr, ents = vcrlog.parse_tsv(logtxt); ev = vcrlog.load_events()
for e in ents:
    if e['seq'] > seq0 and ev.get(e['code'], ('',))[0] in ('HWC_EXCLUSIVE', 'HWC_SLIAA', 'SLI_DONE', 'SLI_STEP', 'MODESET_BEGIN', 'DD_ASSERT_MODE'):
        say('   ', vcrlog.format_entry(e, ev)[:200])
(O / 'owner_sequence.txt').write_text('\n'.join(lines) + '\n')
