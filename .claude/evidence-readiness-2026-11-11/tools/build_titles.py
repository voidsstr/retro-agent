#!/usr/bin/env python3
"""Build scripts/fleet/lanparty_titles.json (+ <evid>/recipes.json) from the
scout's reviewed recipes. One-off for the 2026-11-11 readiness rounds."""
import json, os, re, sys

EVID = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WT = sys.argv[1]
R = {x['key']: x for x in json.load(open(f'{EVID}/recipes_raw.json'))}
ONLINE = {110, 123, 124, 133, 143, 171, 184, 186, 240, 243}
# .124: vcr-kmd README - "no D3D title in an unattended sweep" (Max Payne froze it, 2026-10-04)
D3D124 = {'Halo', 'UT2003', 'UT2004', 'JediKnightMotS', 'RainbowSix', 'Shogo', 'ThiefGold', 'Thief2',
          'SystemShock2', 'MaxPayne', 'RedFaction', 'AliensVsPredator', 'HiddenAndDangerous'}
QUIT_CON = ['TILDE', 'TEXT:quit', 'RETURN']


def o(ip):
    return int(str(ip).split('.')[-1])


def clean(bat):
    out = []
    for ln in bat.replace('\r\n', '\n').split('\n'):
        if re.match(r'(?i)^\s*(rem(\s|$)|::)', ln):
            continue
        out.append(ln.rstrip())
    while out and not out[-1]:
        out.pop()
    s = '\n'.join(out) + '\n'
    s.encode('ascii')
    return s


def bats_of(key, drop=(), only=None):
    out = []
    for l in R[key]['recipe']['launchers']:
        boxes = [o(b) for b in l['boxes'] if o(b) in ONLINE and o(b) not in drop]
        if only is not None:
            boxes = [b for b in boxes if b in only]
        if boxes:
            out.append({'boxes': [str(b) for b in boxes], 'bat': clean(l['bat'])})
    return out


def lb(key, box):
    for l in R[key]['recipe']['launchers']:
        if f'192.168.1.{box}' in l['boxes']:
            return clean(l['bat'])
    raise KeyError((key, box))


def boxes_of(bats, extra=()):
    s = {int(b) for x in bats for b in x['boxes']} | set(extra)
    return sorted(s)


def exes(key):
    return R[key]['recipe']['exe_names']


def wait(key):
    return int(R[key]['recipe'].get('load_wait_sec') or 90)


T = {'_note': 'Generated 2026-11-11 from .claude/evidence-readiness-2026-11-11/recipes_raw.json by tools/build_titles.py. '
              'Each title names its researched recipe(s) in "recipes"; read them for first-run keys, proofs and risks. '
              '.124 is left out of every Direct3D title (vcr-kmd README: no D3D title in an unattended sweep).'}


def add(key, recipes, **kw):
    e = {'recipes': recipes}
    e.update(kw)
    e['boxes'] = [str(b) for b in (kw.get('boxes') or boxes_of(kw['bats'], [o(kw['host']['box'])] if kw.get('host') else []))]
    T[key] = e


# ---- user-named
add('CounterStrike16', ['CounterStrike16'], title='Counter-Strike 1.6', mode='host_server',
    bats=bats_of('CounterStrike16'), query=['goldsrc', 27018], port=27018, exe=['hl.exe'], wait=90, quit9x=QUIT_CON)
add('Quake3-TeamArena', ['Quake3-TeamArena'], title='Quake III Team Arena', mode='host_server',
    bats=bats_of('Quake3-TeamArena'), query=['q3', 27962], port=27962, exe=exes('Quake3-TeamArena'), wait=120,
    prelaunch=[{'rcon': 'map mpteam1', 'port': 27962}, {'sleep': 8}])
ta = lb('Quake3-TeamArena', 110)
q3 = (ta.replace('missionpack\\', 'baseq3\\').replace('missionpack"', 'baseq3"').replace('C:\\fleetmp\\q3ta', 'C:\\fleetmp\\q3a')
        .replace(' +set fs_game missionpack', '').replace(':27962', ':27961').replace('Quake3-TeamArena.bat', 'Quake3Arena.bat'))
assert 'missionpack' not in q3 and ':27961' in q3, q3
add('Quake3Arena', ['Quake3-TeamArena'], title='Quake III Arena (baseq3, q3-server)', mode='host_server',
    note='Derived from the Team Arena recipe launcher: baseq3, profile C:\\fleetmp\\q3a, q3-server :27961.',
    bats=[{'boxes': [str(b) for b in boxes_of(bats_of('Quake3-TeamArena'))], 'bat': q3}],
    query=['q3', 27961], port=27961, exe=exes('Quake3-TeamArena'), wait=120)
# 2026-11-11 round: '-directip +connect <ip>' broadcast its connect on .110/.124/.240; '-directip <ip>' joined .133/.171
d3 = [{'boxes': b['boxes'], 'bat': b['bat'].replace('-directip +connect {host}', '-directip {host}')} for b in bats_of('Descent3')]
add('Descent3', ['Descent3'], title='Descent 3', mode='host_server', bats=d3,
    files=[{'boxes': ['*'], 'path': 'C:\\fleetmp\\{name}.plt', 'gen': 'd3pilot'}],
    query=['gamespy', 20142], roster_ignore=['-Server-'], port=2092, exe=['main.exe'], wait=90)
add('RedAlert2', ['RedAlert2'], title='Red Alert 2', mode='peer_box_host', peer_host='240',
    bats=bats_of('RedAlert2'), exe=['Ra2.exe', 'game.exe'], wait=45)
hb = R['Halo']['recipe']['host_box']
add('Halo', ['Halo'], title='Halo PC', mode='peer_box_host', d3d124=True,
    host={'box': '123', 'bat': clean(hb['command']), 'exe': ['haloded.exe'], 'wait': 30},
    bats=bats_of('Halo', drop={124}), query=['gamespy', 2302], port=2302, exe=['halo.exe'], wait=120)
add('ReturnToCastleWolfenstein', ['ReturnToCastleWolfenstein'], title='Return to Castle Wolfenstein', mode='host_server',
    bats=bats_of('ReturnToCastleWolfenstein'), query=['q3', 27963], port=27963, exe=['WolfMP.exe', 'WolfSP.exe'], wait=120)
t2 = [{'boxes': b['boxes'], 'bat': b['bat'].replace(':28000', ':28001')} for b in bats_of('Tribes2')]
add('Tribes2', ['Tribes2'], title='Tribes 2', mode='host_server',
    note='Joins tribes2-lan, a temporary -nologin instance on :28001 (recipe option B); the main tribes2-server runs -online (TribesNext auth) which no fleet box can join unattended.',
    bats=t2, query=['t2', 28001], port=28001, exe=['Tribes2.exe'], wait=300)
q2 = bats_of('Quake2Complete', drop={243}) + bats_of('Quake2Win9x', only={243})
add('Quake2', ['Quake2Complete', 'Quake2Win9x'], title='Quake II (Quake2Complete on XP, Quake2Win9x on .243)', mode='host_server',
    bats=q2, query=['q2', 27910], port=27910, exe=['quake2.exe'], wait=150, quit9x=QUIT_CON,
    prelaunch=[{'rcon': 'gamemap q2dm1', 'port': 27910}, {'sleep': 5}])
add('Quake1', ['Quake1'], title='Quake (GLQuake)', mode='host_server', bats=bats_of('Quake1'),
    query=['q3', 26000], port=26000, exe=['GLQUAKE.EXE', 'WINQUAKE.EXE'], wait=90, quit9x=QUIT_CON + ['Y'],
    prelaunch=[{'rcon': 'map e1m1', 'port': 26000}, {'sleep': 5}])
ut = (bats_of('UnrealTournament', only={110, 123, 171, 240, 186})
      + bats_of('UnrealTournament436', only={124, 133, 143, 184, 243}))
add('UT99', ['UnrealTournament', 'UnrealTournament436'],
    title='Unreal Tournament (469e client on SSE2 boxes, retail 436 elsewhere)', mode='host_server',
    bats=ut, query=['gamespy', 7798], port=7797, exe=['UnrealTournament.exe'], wait=180, quit9x=QUIT_CON)
add('UT2003', ['UT2003'], title='Unreal Tournament 2003', mode='host_server', d3d124=True,
    bats=bats_of('UT2003', drop={124}), query=['ut2003', 7758], port=7757, exe=['UT2003.exe'], wait=120)
add('UT2004', ['UT2004'], title='Unreal Tournament 2004', mode='host_server', d3d124=True,
    bats=bats_of('UT2004', drop={124}), query=['gamespy', 7787], port=7777, exe=['UT2004.exe'], wait=90)

# ---- 1998-2001 3D
add('HalfLife1', ['HalfLife1'], title='Half-Life (WON 1.1.0.8, listen server on .240)', mode='peer_box_host',
    note='At most 5 players per CD key, host included: run the joiners in waves of 4 with --boxes and --no-host.',
    host={'box': '240', 'bat': lb('HalfLife1', 240), 'exe': ['hl.exe'], 'wait': 60},
    bats=bats_of('HalfLife1', drop={240}), query=['goldsrc', 27015], exe=['hl.exe'], wait=120, quit9x=QUIT_CON)
add('Carmageddon2', ['Carmageddon2'], title='Carmageddon 2', mode='host_server', bats=bats_of('Carmageddon2'),
    exe=['CARMA2_HW.EXE', 'Carma2_SW.exe', 'carma2.exe'], wait=60)
add('JediKnightMotS', ['JediKnightMotS'], title='Jedi Knight: Mysteries of the Sith', mode='peer_box_host', d3d124=True,
    host={'box': '123', 'bat': lb('JediKnightMotS', 123), 'exe': ['JKM.EXE'], 'wait': 45},
    bats=bats_of('JediKnightMotS', drop={123, 124}), exe=['JKM.EXE'], wait=45)
add('RainbowSix', ['RainbowSix'], title='Rainbow Six', mode='peer_box_host', d3d124=True,
    host={'box': '123', 'bat': lb('RainbowSix', 123), 'exe': ['RainbowSix.exe'], 'wait': 60},
    bats=bats_of('RainbowSix', drop={123, 124}), exe=['RainbowSix.exe'], wait=90)
add('Shogo', ['Shogo'], title='Shogo', mode='host_server', d3d124=True, bats=bats_of('Shogo', drop={124}),
    query=['gamespy', 27888], port=27888, exe=['Client.exe', 'Shogo.exe'], wait=90)
add('SiNGold', ['SiNGold'], title='SiN Gold (listen server on .123)', mode='peer_box_host',
    host={'box': '123', 'bat': clean(R['SiNGold']['recipe']['host_box']['command']), 'exe': ['sin.exe'], 'wait': 90},
    bats=bats_of('SiNGold', drop={123}), query=['q2', 22450], exe=['sin.exe'], wait=240, quit9x=QUIT_CON)
add('ThiefGold', ['ThiefGold'], title='Thief Gold (single player)', mode='sp_only', d3d124=True,
    bats=bats_of('ThiefGold', drop={124}), exe=['THIEF.EXE'], wait=45)
add('Turok2', ['Turok2'], title='Turok 2 (integrated server on .110)', mode='peer_box_host',
    host={'box': '110', 'bat': lb('Turok2', 110), 'exe': exes('Turok2'), 'wait': 60},
    bats=bats_of('Turok2', drop={110}), query=['gamespy', 12888], exe=exes('Turok2'), wait=60)
add('UnrealGold', ['UnrealGold'], title='Unreal Gold', mode='host_server', bats=bats_of('UnrealGold'),
    query=['gamespy', 7808], port=7807, exe=['Unreal.exe'], wait=120, quit9x=QUIT_CON)
add('DieByTheSword', ['DieByTheSword'], title='Die by the Sword (single player, .243 only)', mode='sp_only',
    bats=bats_of('DieByTheSword'), exe=['windie.exe'], wait=60)
avp = lb('AliensVsPredator', 110)
add('AliensVsPredator', ['AliensVsPredator'], title='Aliens vs Predator Gold (launch check - library tree known broken)',
    mode='broken_known', d3d124=True,
    bats=[{'boxes': [str(b) for b in sorted(ONLINE & {110, 123, 133, 143, 171, 184, 240})], 'bat': avp}], exe=['avp.exe'], wait=45)
add('SystemShock2', ['SystemShock2'], title='System Shock 2 (single player)', mode='sp_only', d3d124=True,
    bats=bats_of('SystemShock2', drop={124}), exe=exes('SystemShock2'), wait=75)
add('RogueSquadron', ['RogueSquadron'], title='Rogue Squadron 3D (single player, .243 only)', mode='sp_only',
    bats=bats_of('RogueSquadron'), exe=exes('RogueSquadron'), wait=75)
add('HiddenAndDangerous', ['HiddenAndDangerous'], title='Hidden and Dangerous Deluxe (launch check - MP is attended-only)',
    mode='mp_not_automatable', d3d124=True, bats=bats_of('HiddenAndDangerous', drop={124}), exe=['HDE.exe'], wait=60)
add('DeusEx', ['DeusEx'], title='Deus Ex', mode='host_server', bats=bats_of('DeusEx'),
    query=['gamespy', 7791], port=7790, exe=['DeusEx.exe'], wait=180)
add('SoldierOfFortune', ['SoldierOfFortune'], title='Soldier of Fortune (launch check - known broken)', mode='broken_known',
    bats=bats_of('SoldierOfFortune'), exe=['SoF.exe'], wait=90)
add('Thief2', ['Thief2'], title='Thief II (single player)', mode='sp_only', d3d124=True,
    bats=bats_of('Thief2', drop={124}), exe=['Thief2.exe'], wait=45)
add('MaxPayne', ['MaxPayne'], title='Max Payne (single player)', mode='sp_only', d3d124=True,
    bats=bats_of('MaxPayne', drop={124}), exe=['MaxPayne.exe'], wait=90)
add('RedFaction', ['RedFaction'], title='Red Faction (dedicated server on .123)', mode='peer_box_host', d3d124=True,
    host={'box': '123', 'bat': clean(R['RedFaction']['recipe']['host_box']['command']), 'exe': ['rf.exe'], 'wait': 30},
    bats=bats_of('RedFaction', drop={124}), exe=['rf.exe', 'RedFaction.exe'], wait=90)
add('SeriousSamFirstEncounter', ['SeriousSamFirstEncounter'], title='Serious Sam: The First Encounter', mode='host_server',
    bats=bats_of('SeriousSamFirstEncounter'), query=['gamespy', 25601], port=25600, exe=['SeriousSam.exe'], wait=90)
add('SeriousSamSecondEncounter', ['SeriousSamSecondEncounter'], title='Serious Sam: The Second Encounter', mode='host_server',
    bats=bats_of('SeriousSamSecondEncounter'), query=['gamespy', 25611], port=25610, exe=['SeriousSam.exe'], wait=120)

# ---- checks
problems = []
for k, e in T.items():
    if k.startswith('_'):
        continue
    for b in e.get('bats', []) + ([e['host']] if e.get('host') else []):
        txt = b['bat']
        if '243' in b.get('boxes', []):
            for ln in txt.splitlines():
                if len(ln.replace('{name}', 'Fleet243').replace('{host}', '192.168.1.132')) > 126:
                    problems.append(f'{k}: .243 line over 126 chars: {ln[:60]}')
            if re.search(r'(?i)%~|cd /d|start ""', txt):
                problems.append(f'{k}: .243 bat uses cmd.exe syntax')
    for b in e.get('bats', []):
        if not b['boxes']:
            problems.append(f'{k}: empty boxes')
json.dump(T, open(os.path.join(WT, 'scripts/fleet/lanparty_titles.json'), 'w'), indent=1)
recipes = {k: dict(v['recipe'], review_issues=v.get('review_issues')) for k, v in R.items()}
json.dump(recipes, open(f'{EVID}/recipes.json', 'w'), indent=1)
print('titles:', len([k for k in T if not k.startswith('_')]))
for k, e in T.items():
    if not k.startswith('_'):
        print(f"  {k:26s} boxes={','.join(e['boxes'])}  host={(e.get('host') or {}).get('box', '-')}")
print('problems:', problems or 'none')
