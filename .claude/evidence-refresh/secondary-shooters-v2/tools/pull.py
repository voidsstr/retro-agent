#!/usr/bin/env python3
"""DOWNLOAD (read-only) small per-box config/log files for the 16 titles and
save them under boxes/<box>/<Title>/... XP/7 boxes only (never .243)."""
import asyncio, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EVID = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/secondary-shooters-v2/boxes'
FILES = [
    r'UnrealGold\System\Unreal.ini', r'UnrealGold\System\Unreal.log',
    r'UnrealTournament\System\UnrealTournament.ini', r'UnrealTournament\System\UnrealTournament.log',
    r'UnrealTournament436\System\UnrealTournament.ini', r'UnrealTournament436\System\UnrealTournament.log',
    r'DeusEx\System\DeusEx.ini', r'DeusEx\System\DeusEx.log',
    r'UT2004\System\UT2004.ini', r'UT2004\System\UT2004.log',
    r'Shogo\autoexec.cfg', r'Shogo\Play Shogo - direct engine.bat',
    r'SeriousSamFirstEncounter\Scripts\PersistentSymbols.ini', r'SeriousSamFirstEncounter\Scripts\Game_startup.ini',
    r'SeriousSamFirstEncounter\SeriousSam.log',
    r'SeriousSamSecondEncounter\Scripts\PersistentSymbols.ini', r'SeriousSamSecondEncounter\Scripts\Game_startup.ini',
    r'SeriousSamSecondEncounter\SeriousSam.log',
    r'Halo\Play Halo.bat',
    r'FarCry\System.cfg', r'FarCry\game.cfg', r'FarCry\log.txt',
    r'Tribes2\GameData\base\prefs\ClientPrefs.cs', r'Tribes2\GameData\console.log',
    r'BF1942\Mods\bf1942\Settings\Profiles\Default\Video.con',
    r'BF1942\Mods\bf1942\Settings\Profiles\Custom\Video.con',
    r'Turok2\Data\config.ned', r'Turok2\turok2.ini',
    r'Descent3\Play Descent 3.bat',
]
async def main(ip, files):
    tag = ip.split('.')[-1]
    if tag == '243':
        raise SystemExit('not on .243')
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=15)
    try:
        for rel in files:
            path = 'C:\\Games\\' + rel
            st, d = await c.send_command('DOWNLOAD ' + path)
            out = os.path.join(EVID, tag, *rel.split('\\'))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if not d.startswith(b"Cannot open file"):
                open(out, 'wb').write(d)
                print('%s OK   %7d  %s' % (tag, len(d), rel))
            else:
                open(out + '.MISSING', 'w').write('status=%s %s' % (st, d[:200].decode('latin-1', 'replace')))
                print('%s MISS st=%s  %s  (%s)' % (tag, st, rel, d[:60]))
    finally:
        await c.close()
ip = sys.argv[1]
asyncio.run(main(ip, sys.argv[2:] or FILES))
