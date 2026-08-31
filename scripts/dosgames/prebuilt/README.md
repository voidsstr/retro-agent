# `DOSGAME_2026-08-26.exe` — a binary we cannot rebuild

**This directory exists to stop us losing a program whose source is gone.**

`DOSGAME_2026-08-26.exe` (113,012 bytes, md5 `06e0615d37f800eb2d08303606c0bc8a`) is
the `DOSGAME.EXE` the fleet actually runs. It is **not** what this repo builds:

| | size |
|---|---|
| `git HEAD`, rebuilt (byte-exact, reproducible) | 111,170 B |
| **published on the share, 2026-08-26** | **113,012 B** |

The extra 1,842 bytes are real work. They are a refinement to how `DOSGAME`
picks a game's launcher, and a registry-repair rule, and their source exists in
**no commit, no branch, no worktree and no file on this host** — searched with
`git log --all -S` over the whole history with no path filter, `git grep` across
every reachable commit, and a filesystem sweep. The only commit that matches
those strings is `6ae3be9`, the report *about* their absence.

`DOSGAME.BAK` on the share is an **older** build (107,284 B, 2026-08-25) and does
**not** contain the feature, so it is not a second copy. Before this directory
existed there was exactly one, on a share that CLAUDE.md records has already been
rebuilt once and silently lost its whole `Utility\Retro Automation\` tree.

## What the lost feature does, read out of the binary

Recovered from its own log strings — enough to reimplement deliberately, and
deliberately **not** reimplemented from guesswork, because a wrong
reconstruction would look like a restoration:

```
pick:   %s is a self-extracting archive, not the game
pick:   %s -> %s (self-extracting archive; needs setup run)
pick:   %s -> %s (skip-listed, but it is the only thing that runs here)
registry: DROP %s - launcher "%s" is a self-extracting archive, not the game;
          re-deriving
```

So it: recognises a self-extracting archive and refuses to treat it as the game;
overrides its own skip-list when the skipped file is the only runnable thing in
the directory; and repairs `INSTALL.LST` by dropping an entry whose recorded
launcher was one of those archives, re-deriving it instead.

## The trade, unresolved on purpose

Publishing this repo's build would ship the staged-launcher work (`DOSGAME.TXT`)
and **permanently delete** the above. Not publishing leaves `DOSGAME.TXT` inert
on the boxes. Neither is obviously right, so nothing was published and the
artifact was preserved instead — the reversible half of the decision.

`scripts/dosgames/check-published.py` (also `make check-published`) reports
whether the fleet runs what the repo builds. It **reports and does not fail**:
the divergence is known and unresolved, and failing every `run_all.sh` over it
would only train people to ignore the suite. `--strict` is there for the day it
is resolved.

## If you resolve it

Reimplement the feature from the strings above, confirm the rebuilt binary
contains all four, then publish and delete this directory. Do **not** publish a
build that lacks them without saying so explicitly — that is a silent
regression on the DOS boxes, and its only symptom is a game menu that launches
an installer instead of a game.
