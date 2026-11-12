import asyncio, sys, json, time
sys.path.insert(0,'/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
from PIL import Image
import io
EV='/home/voidsstr/development/retro-agent/.claude/evidence-refresh/124-window/'
IP='192.168.1.124'
TESTS=[x.split('|') for x in sys.argv[1:]]   # name|dir|bat|exe1,exe2
def ts(): return time.strftime('%H:%M:%S')
async def cmd(c,s,t=60):
    st,d=await asyncio.wait_for(c.send_command(s),timeout=t); return d.decode('latin1','replace')
async def procs(c):
    p=json.loads(await cmd(c,'PROCLIST')); p=p if isinstance(p,list) else p.get('processes',[]); return {x.get('name','').lower():x.get('pid') for x in p}
async def log_events(c,tag):
    await cmd(c,f'EXEC cmd /c C:\\vcr\\vcrctl.exe log > C:\\vcr\\rq_{tag}.txt 2>&1')
    st,d=await c.send_command(f'DOWNLOAD C:\\vcr\\rq_{tag}.txt'); open(EV+f'rq_{tag}.txt','wb').write(d)
    await cmd(c,f'EXEC cmd /c del C:\\vcr\\rq_{tag}.txt')
    return d.decode('latin1')
async def main():
    c=RetroConnection(IP,9898); await c.connect('retro-agent-secret',timeout=15)
    base=await log_events(c,'base'); seq0=max(int(l.split('\t')[0]) for l in base.splitlines() if l[:1].isdigit())
    print(ts(),'base seq',seq0, await cmd(c,'DISPLAYCFG get'))
    for name,d,bat,exes in TESTS:
        exes=[e.lower() for e in exes.split(',')]
        print('=====',ts(),name)
        await cmd(c,f'EXEC cmd /c start "" /D "{d}" "{bat}"')
        await asyncio.sleep(25)
        pr=await procs(c); run=[e for e in exes if e in pr]
        print(ts(),'running:',run,'DISPLAYCFG',await cmd(c,'DISPLAYCFG get'))
        w=json.loads(await cmd(c,'WINLIST')); print(' fg:',w.get('foreground',{}).get('title'),[ (x['title'][:30],x['rect']) for x in w['windows'] if x['rect']['left']>-30000][:4])
        try:
            bmp=await c.command_binary('SCREENSHOT 1'); Image.open(io.BytesIO(bmp)).save(EV+f'{name}.png')
        except Exception as e: print(' shot err',e)
        lg=await log_events(c,name)
        for l in lg.splitlines():
            f=l.split('\t')
            if f[0].isdigit() and int(f[0])>seq0 and f[4] in ('300','301','604','603','605','606'): print('  ev',l[:160])
        seq0=max(int(l.split('\t')[0]) for l in lg.splitlines() if l[:1].isdigit())
        if run:
            for k in ('UIKEY TILDE','SLEEP','UIKEY TEXT:quit','UIKEY RETURN'):
                if k=='SLEEP': await asyncio.sleep(1.5); continue
                print(' key',k,await cmd(c,k))
        for i in range(5):
            await asyncio.sleep(4); pr=await procs(c)
            if not any(e in pr for e in exes): break
        left=[e for e in exes if e in pr]
        if left:
            w=json.loads(await cmd(c,'WINLIST')); print(' console-quit did not end it; fg:',w.get('foreground'))
            for e in left: print(' close',e,(await cmd(c,f'EXEC taskkill /im "{e}"')).strip()[:100])
        for i in range(6):
            await asyncio.sleep(5); pr=await procs(c)
            if not any(e in pr for e in exes): break
        left=[e for e in exes if e in pr]
        print(ts(),'after close left:',left,'DISPLAYCFG',await cmd(c,'DISPLAYCFG get'))
        if left: print('!!! still running, stopping sequence'); break
        await asyncio.sleep(6)
    lg=await log_events(c,'end')
    for l in lg.splitlines():
        f=l.split('\t')
        if f[0].isdigit() and int(f[0])>seq0 and f[4] in ('300','301','604'): print('  ev',l[:160])
    print(ts(),'final',await cmd(c,'DISPLAYCFG get'))
    await c.close()
asyncio.run(main())
