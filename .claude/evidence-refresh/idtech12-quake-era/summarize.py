import json, sys, glob, os
TITLES = ['Quake1','Quake2Complete','Quake2Win9x','HexenII','SiNGold','SoldierOfFortune']
for path in sorted(glob.glob('boxes/*.json')):
    txt = open(path).read()
    # file may contain several JSON documents appended
    dec = json.JSONDecoder(); i = 0; docs = []
    while i < len(txt):
        while i < len(txt) and txt[i].isspace(): i += 1
        if i >= len(txt): break
        d, j = dec.raw_decode(txt, i); docs.append(d); i = j
    print('=====', path)
    for d in docs:
        for k, v in d.items():
            if k.startswith('DIRLIST') and isinstance(v, dict) and 'text' in v:
                try:
                    lst = json.loads(v['text'])
                    names = [e['name'] for e in lst]
                    hits = [n for n in names if n.lower() in [t.lower() for t in TITLES]]
                    print(k, '-> relevant:', hits, '(total entries %d)' % len(names))
                except Exception as e:
                    print(k, 'unparsed', v['text'][:300])
            elif k.startswith('GAMERES') and isinstance(v, dict):
                print(k, v.get('text','')[:3000])
            elif k != '_greeting':
                print(k, (v.get('text','') if isinstance(v, dict) else v)[:1500])
            else:
                print(k, v)
