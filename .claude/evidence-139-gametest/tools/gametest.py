import asyncio,sys,io,json,os,re
sys.path.insert(0,'/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
from PIL import Image
OUT='/home/voidsstr/development/retro-agent/.claude/evidence-139-gametest'
SUF=os.environ.get('SUF','')
DIR=r'C:\Users\Public\Desktop\Retro Games'
IGN={'svchost.exe','runtimebroker.exe','searchprotocolhost.exe','searchfilterhost.exe','backgroundtaskhost.exe','dllhost.exe','wmiprvse.exe','audiodg.exe','smartscreen.exe','taskhostw.exe','sppsvc.exe','compattelrunner.exe','msedge.exe','nvcontainer.exe','nvidia share.exe','nvdisplay.container.exe','onedrive.exe','sihost.exe','ctfmon.exe','trustedinstaller.exe','tiworker.exe','mousocoreworker.exe','usocoreworker.exe','wudfhost.exe','searchindexer.exe','securityhealthservice.exe','msmpeng.exe','mpcmdrun.exe','nissrv.exe','retro_agent.exe','opera.exe','explorer.exe','applicationframehost.exe','systemsettings.exe','shellexperiencehost.exe','searchapp.exe','startmenuexperiencehost.exe','textinputhost.exe','gamebar.exe','gamebarftserver.exe','gamebarpresencewriter.exe','backgroundtransferhost.exe','sgrmbroker.exe','spoolsv.exe','lsass.exe','csrss.exe','winlogon.exe','dwm.exe','fontdrvhost.exe','robloxplayerbeta.exe','steam.exe','steamwebhelper.exe','battle.net.exe','agent.exe','discord.exe','epicgameslauncher.exe','powershell.exe','conhost.exe_'}
async def cmd(c,s,binary=False):
    if binary: return await c.command_binary(s)
    st,d=await c.send_command(s); return d.decode('utf-8','replace')
async def conn():
    c=RetroConnection('192.168.1.139',9898); await c.connect('retro-agent-secret',timeout=20); return c
async def procs(c): return {p['pid']:p['name'] for p in json.loads(await cmd(c,'PROCLIST'))}
async def test(name,wait,target):
    slug=re.sub(r'[^A-Za-z0-9]+','_',name).strip('_')
    c=await conn(); base=await procs(c); r={'name':name}
    r['launch']=(await cmd(c,f'LAUNCH {target}'))[:200]; await c.close()
    await asyncio.sleep(wait)
    c=await conn(); now=await procs(c)
    new={p:n for p,n in now.items() if p not in base and n.lower() not in IGN}
    r['new']=sorted(set(new.values())); r['mode']=await cmd(c,'DISPLAYCFG get')
    wl=json.loads(await cmd(c,'WINLIST'))
    r['wins']=[(w['title'],w['class'],w['rect']) for w in wl['windows'] if w['pid'] in new or w['class']=='#32770']
    try:
        d=await cmd(c,'SCREENSHOT 1',True); im=Image.open(io.BytesIO(d)); im.save(f'{OUT}/{slug}{SUF}.png'); r['extrema']=im.convert('L').getextrema()
    except Exception as e: r['shot_err']=str(e)
    for _ in range(2):
        for p,n in new.items():
            if n.lower() in ('cmd.exe','conhost.exe') and _==0: continue
            await cmd(c,f'EXEC taskkill /f /t /pid {p}')
    await c.close(); await asyncio.sleep(6)
    c=await conn(); r['mode_after']=await cmd(c,'DISPLAYCFG get')
    if '2560' not in r['mode_after']:
        await cmd(c,r'EXEC powershell -NoProfile -ExecutionPolicy Bypass -File C:\RETRO_AGENT\resmode.ps1'); r['mode_fixed']=await cmd(c,'DISPLAYCFG get')
    await c.close(); return r
async def main():
    for line in open(sys.argv[1]):
        line=line.strip()
        if not line: continue
        parts=line.split('|'); name=parts[0]; w=int(parts[1]) if len(parts)>1 and parts[1] else 30
        target=parts[2] if len(parts)>2 else f'cmd /c start "" "{DIR}\\{name}.lnk"'
        try: r=await test(name,w,target)
        except Exception as e: r={'name':name,'error':repr(e)}
        open(f'{OUT}/results{SUF}.jsonl','a').write(json.dumps(r)+'\n')
        print(r['name'],r.get('new'),r.get('mode'),[x[0][:50] for x in r.get('wins',[])],r.get('mode_fixed',''),r.get('error',''),flush=True)
asyncio.run(main())
