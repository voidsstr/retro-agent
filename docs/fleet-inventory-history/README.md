# Fleet hardware history

`docs/fleet-inventory.md` is regenerated, and therefore overwritten, on every
run. This directory is what remembers. Each subfolder is one dated snapshot
(UTC, `YYYY-MM-DDTHHMMZ`) taken by

    python3 scripts/fleet/inventory.py --snapshot

and holds:

- `records/<ip>_<hostname>.json` - every box's hardware record exactly as it
  published it (`HWPROFILE` via `agent/src/hwpublish.c`), plus
  `records/unrostered/` for records on the share that match no roster entry
  (usually an earlier identity of a re-imaged box);
- `summary.json` - one row per rostered box: state, when measured, CPU, RAM,
  active GPU, every video card, 3dfx accelerators, OS, profile hash;
- `fleet-inventory.md` - the rendered document as of that moment.

**Take one before and after every hardware swap.** Each box measures itself
when its agent starts; to force a fresh record first, send `HWPUBLISH` to it.
A powered-off box keeps its last record, and the snapshot says how old it is
(`state` / `measured`), so "current" and "last seen" are never confused.
