# `scripts/perbox/` — the per-(box, title) verification sweep

This is the only thing on the fleet that answers **"does this title actually
render on this machine?"** Every other source answers a weaker question — is the
tree on the disk (`DIRLIST`, `installed_games`), or would the gate allow it
(`gamegate.db`). Those cannot tell a game that runs from a game that is merely
*present*, and this project's whole failure history is tools reporting success.

Output lands in `fleetbook.db` as `origin='measured'`, `source='perbox'` rows,
written through `scripts/fleet/compat_db.py`'s own `put_*` helpers — this
directory **does not define a schema**. See
[`../fleet/README-compat.md`](../fleet/README-compat.md).

## The files

| file | what it does |
|---|---|
| `fleetlib.py` | shared `Box` wrapper + the box list. **See the path note below.** |
| `measure.py` | measures ONE cell: launches, waits, reads back actual resolution / refresh / fullscreen / renderer, takes a screenshot. An untested cell is never recorded as a pass. |
| `sweep.py` | walks the matrix. **Resumable** — appends JSONL and skips cells already done, so an interrupted run costs nothing. Skips titles a sibling agent is actively editing (measuring those races the edit). |
| `retest.py` | re-measures only the FAILED cells at a 90 s settle. A 30 s settle is a *measurement artefact*, not a property of a game that plays a publisher movie, builds a shader cache or scans a CD. |
| `desktopcheck.py` | finds FALSE PASSES by comparing each cell's frame against that box's own desktop. |
| `report.py` | summarises what was **measured**, never what was assumed. |
| `to_compat.py` | loads the matrix into the compat tables. |
| `state.json`, `state_gate.json` | the last sweep's raw state, kept so a claim can be traced to the run that made it. |

## Run it

```bash
python3 scripts/perbox/sweep.py /tmp/matrix.jsonl     # resumable; safe to re-run
python3 scripts/perbox/retest.py /tmp/matrix.jsonl    # only the failures, longer settle
python3 scripts/perbox/desktopcheck.py /tmp/matrix.jsonl
python3 scripts/perbox/report.py /tmp/matrix.jsonl
python3 scripts/perbox/to_compat.py /tmp/matrix.jsonl
```

Run `desktopcheck.py` **before** `to_compat.py`. Loading an unchecked matrix puts
false passes into the database, and a `measured` row is deliberately not
overwritten by a later ingest — so a bad one has to be deleted by hand.

## Two things that will bite you

**A process in `PROCLIST` is not evidence, and the sharp version is worse than
the usual one.** A game that dies with an illegal instruction (`0xC000001D`)
**keeps its name in the process list**, held by Windows Error Reporting. The
cell scores `runs` while nothing was ever drawn. UnrealTournament 469e on `.124`
and `.133` does exactly this. That is what `desktopcheck.py` is for; skipping it
is how a false `verified` gets into the database.

**`fleetlib.py` hard-codes a worktree path.**

```python
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/perbox')
```

It needs `client/retro_protocol.py`, and it names the worktree the sweep was
written in rather than the one you are running from. If that worktree is ever
removed the import dies with `ModuleNotFoundError: client`, which reads like a
missing dependency and is not. Point it at your own checkout, or export
`PYTHONPATH` to a tree that has `client/`. It is left as-is rather than
"fixed" blind because the recorded sweeps were taken with it.

## Testing must be fullscreen

Every measurement here is taken **fullscreen** — see the fullscreen rule in
`CLAUDE.md`. A windowed frame measures the window manager, not the game.
