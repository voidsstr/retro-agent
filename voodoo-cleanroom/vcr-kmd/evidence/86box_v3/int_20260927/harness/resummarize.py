#!/usr/bin/env python3
"""Rebuild flip_summary.jsonl / d3d_summary.jsonl from the saved per-run
.vcrlog.tsv with the correct window (seq > seq0: vcrctl info's log_next_seq
is the LAST seq written). The first pass used seq >= seq0 and could credit a
run with the previous run's last entry."""
import json, sys
from pathlib import Path
KMD = Path('/home/voidsstr/development/retro-agent/.claude/worktrees/vk-int/voodoo-cleanroom/vcr-kmd')
sys.path.insert(0, str(KMD / 'tools'))
import vcrlog
ev = vcrlog.load_events()
for summ in sorted((KMD / 'evidence/86box_v3/int_20260927').rglob('*_summary.jsonl')):
    rows = [json.loads(l) for l in summ.read_text().splitlines() if l.strip()]
    out = []
    for r in rows:
        hdr, ents = vcrlog.parse_tsv((summ.parent / f"{r['label']}.vcrlog.tsv").read_text())
        ents = [e for e in ents if e['seq'] > r['seq0']]
        r['warn_err'] = [vcrlog.format_entry(e, ev) for e in ents if e['level'] <= 1]
        if summ.name == 'flip_summary.jsonl':
            r['flip_stats'] = [{'seq': e['seq'], 'a': e['a'], 'msg': e['msg']} for e in ents
                               if e['code'] == 511 and e['a'] in (12, 13)]
            r['modesets'] = [e['msg'] for e in ents if ev.get(e['code'], ('',))[0] in
                             ('MODESET_BEGIN', 'MODESET_PLL') and ('mode ' in e['msg'] or 'pll' in e['msg'])]
        else:
            refuse = [e for e in ents if (e['code'] == 513 and e['a'] in (12, 15, 17)) or
                      (e['code'] == 511 and e['a'] == 11)]
            r['refusals'] = [vcrlog.format_entry(e, ev) for e in refuse]
            r['d3d_ctx'] = [vcrlog.format_entry(e, ev) for e in ents if e['code'] == 513 and e['a'] in (1, 2, 16)]
            layers = sorted({f"{e['code']}/{e['a']}" for e in refuse})
            r['refusing_layer'] = layers or ['none in the driver (no 513/12, 513/15, 513/17 or 511/11 logged)']
        r['window'] = f"seq > {r['seq0']}"
        out.append(r)
    summ.write_text(''.join(json.dumps(r) + '\n' for r in out))
    print(summ.relative_to(KMD), len(out), 'rows')
    for r in out:
        if summ.name == 'd3d_summary.jsonl':
            print('  ', r['label'], (r['result'] or {}).get('error') or (r['result'] or {}).get('hr') or
                  f"pass {(r['result'] or {}).get('pass')} fail {(r['result'] or {}).get('fail')}", r['refusing_layer'])
        else:
            print('  ', r['label'], [s['msg'] for s in r['flip_stats'] if s['a'] == 12])
