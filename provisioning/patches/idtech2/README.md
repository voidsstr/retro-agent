# id Tech 2: 1920x1080 as mode 9 (Quake II 3.20, SiN 1.11, Soldier of Fortune)

These three engines have a fixed video mode table compiled into the exe. It has
ten `{desc, width, height, mode}` entries, from `Mode 0: 320x240` to
`Mode 9: 1600x1200`. There is no custom mode and no 16:9 entry. The in-game
video menu is an index into that table, and `gl_mode` is that index, so no
config value can select 1920x1080. This patch rewrites the last entry as
1920x1080. No fleet box resolves to 1600x1200 today. The patch also relabels
every menu that shows that entry.

`apply.py` rebuilds everything from the staged originals and refuses any file
whose md5, size or original bytes differ from what was measured. Game files are
copyrighted, so its outputs go to `~/.retro-fleet/patch-out/idtech2/` and are
never committed.

```bash
python3 provisioning/patches/idtech2/apply.py --check     # verify originals, show every edit
python3 provisioning/patches/idtech2/apply.py --build     # write outputs + manifest.json
python3 provisioning/patches/idtech2/apply.py --publish   # FUTURE: back up originals, sharewrite one by one
python3 -m pytest -q tests/python/test_patch_idtech2.py
```

## Edits (all same-length, file offsets)

| title | file | edit |
|---|---|---|
| Quake2Complete | `quake2.exe` (md5 57dd2cf4…, 362,496 B) | `vid_modes[9]` width @0x54464 1600→1920, height @0x54468 1200→1080, `Mode 9: 1600x1200`→`Mode 9: 1920x1080` @0x54470, Video-menu label `[1600 1200]`→`[1920 1080]` @0x54720 (label array @0x54678, entry 9, NULL-terminated) |
| SiNGold | `sin.exe` (md5 8d320d7f…, 667,648 B) | width @0x944e4, height @0x944e8, desc string @0x944f0 |
| SiNGold | NEW `base\menus\main.mnu` | copy of `base\pak5.sin:menus/main.mnu`, con_vidmode item 10 `" 1600 x 1200"`→`" 1920 x 1080"` |
| SiNGold | NEW `2015\menus\main.mnu` | copy of `2015\pak0.sin:menus/main.mnu`, same relabel **and `numitems 9`→`10`**. Wages of SiN listed ten strings but declared nine, so its menu stopped at 1280 x 960. |
| SiNGold | NEW `ctf\menus\main.mnu` | copy of `ctf\pak1.sin:menus/main.mnu` (pak1 beats ctf's pak0), same relabel. ctf\ has no shortcut, but a client that joins a `ds_sinctf.bat` server is switched into the server's game dir, and ctf\ sits above base\, so its pak1 copy would otherwise mislabel mode 9. Added in review. |
| SoldierOfFortune | `SoF.exe` (md5 2f4db377…, 2,052,096 B) | width @0x1c6e9c, height @0x1c6ea0, desc string @0x1c6ea8 |
| SoldierOfFortune | NEW `base\pak2.pak` | PACK containing only `menus/m_video.rmf` = pak0's copy with the list `…,1280x960,1600x1200` → `…,1280x960,1920x1080` (the `match "3,…,9"` mapping is unchanged) |

Why loose files for SiN and a pak for SoF: SiN's `FS_AddGameDirectory`
(@0x441ef0) loads pak0-31 and then prepends the loose directory, so a loose
file wins. SoF prepends the loose directory first and then pak0-9, so there a
pak wins and a loose `m_video.rmf` would be shadowed by `pak0.pak`. Both
findings come from disassembly, recorded in
`.claude/evidence-1080p/review-idtech12-doom3/static/` and re-derived in review
(`.claude/evidence-1080p/build-idtech2/review/05-fs-order-disassembly.txt`).

Every pak is prepended as it is opened, so within a game dir the
highest-numbered pak carrying a file is the one the engine loads. `apply.py`
re-derives that winner from the share on every run, and refuses if it is not
the pak the relabel was built from. `base\` has `main.mnu` in pak2, pak4 and
pak5, so pak5 is the source. The `locale\en\menus\` trees in `2015\` and
`ctf\` are not read: no SiN 1.11 binary or pak contains the string `locale`.

**Quake2Win9x keeps the stock `quake2.exe`.** It is the same binary. It serves
the 640x480 Win9x/Voodoo lane, and the OS gate keeps it off every LCD.

## Nothing selects mode 9 until the launchers change

On its own the patch is inert. Every box resolves `FR_Q2MODE` to 8 or less, so
the patched exes behave like the stock ones except for the menu label. The
game starts at 1920x1080 only once the launchers and GAMERES hand these three
titles the new value below. That is why the exes can ship first.

### FR_Q2WIDE (FLEETRES) == `t->q2wide` / `%Q2WIDE%` (GAMERES)

```
q2wide = 9   if the target (after ResCap) is 16:9 (aspect_class_mode == 169),
             target_w >= 1920, target_h >= 1080, and the driver offers
             1920x1080 (mode_offered / gr_mode_offered)
       = q2_mode_for(min(w43, 1280), min(h43, 960))   otherwise
```

The second line is the same selector `FR_Q2MODE` uses, with the table capped at
entry 8. On every box today that is exactly `FR_Q2MODE`. It can never be 9,
which matters because on a patched exe 9 no longer means 1600x1200: a 4:3 box
whose `FR_Q2MODE` would be 9 (.124's P1120 if its desktop were raised to
1600x1200) must not be handed a 16:9 mode.

It re-runs the selector rather than clamping to 8 (the first draft's
`min(q2mode, 8)`). A clamp would hand a 1600x1200 tube that does not list
1280x960 a mode its driver refuses. The id Tech 2 `ref_gl` answers a refused
fullscreen mode by opening a window at that size.

The reference implementation is `apply.fr_q2wide()`, and the per-box
expectations are pinned in the test against the driver mode lists the boxes
reported on 2026-09-29:

| box | panel | target | FR_Q2MODE | FR_Q2WIDE |
|---|---|---|---|---|
| .123 | DELL P2312H LCD | 1920x1080 | 8 | **9** |
| .145 | DELL E2414H LCD | 1920x1080 | 8 | **9** (SiN's GL path crashes there; its software launcher keeps FR_Q2MODE) |
| .195 | HP 2511 LCD | 1920x1080 | 8 | **9** |
| .240 | DELL E2313H LCD | 1920x1080 | 8 | **9** |
| .124 | HP P1120 CRT | 1280x960 | 8 | 8 |
| .143 | no EDID, so treated as a 4:3 CRT | 1280x960 (expected, unmeasured) | 8 | 8 (never 9) |

The SiN software-renderer launcher keeps `sw_mode %FR_Q2MODE%`. Never send
ref_soft to mode 9. Two things make this safe today:
- no box resolves `FR_Q2MODE` to 9;
- `base\autoexec.cfg` sets `sw_mode "6"` later and wins anyway.

If that launcher is ever changed so its sw_mode sticks, it needs a value capped
at 8, not `FR_Q2MODE`.

### The refresh lane must follow this mode

With these exes patched, a 1080p box runs Quake II and SiN at 1920x1080, not at
the 4:3 target. The refresh lane (worktree `refresh-mech`) plans `REFRESHKEEP`
at `%FR_HZ43%` for these launchers, and its `FR_HZQ2` is computed at
`q2tab[FR_Q2MODE]`. Both are rates at 1280x960. The measurement shows why that
is wrong here: on .123, .145 and .240 the driver lists 1280x960 at **75 Hz**
and 1920x1080 only at **60 Hz**, so FR_HZ43 would ask for 1920x1080@75.

For these three titles the rate must be taken at `apply.q2wide_res(q2wide)`:
1920x1080 when `q2wide` is 9, else `Q2TAB[q2wide]`. Evidence:
`.claude/evidence-1080p/build-idtech2/review/06-refresh-cross-lane.txt`.

### What a user can still reach from the menu

The relabelled entry is offered on every box, CRTs included. .124's P1120 does
not list 1920x1080. Picking it on a box that cannot show 1920x1080 does **not**
revert gl_mode. The stock `ref_gl` in all three trees prints "...setting
windowed mode" and "fullscreen unavailable in this mode", sets
`vid_fullscreen 0`, and opens a 1920x1080 **window**. The next launch through
the fleet launcher writes `vid_fullscreen 1` and the box's own mode back.

Quake II's video menu shows the same list for the software renderer too. Its
3.20 `ref_soft.dll` has no width clamp in its code, and id's source sizes the
warp buffers for 1600, so software plus the 1920 entry is untested and probably
crashes.

## Publishing (future)

`--publish` works as follows:
- It refuses unless `--library` is the share mount that `sharewrite.py`
  verifies through.
- It backs up each original exe first, then puts one file at a time, stopping
  on the first failure.
- A re-run resumes. A destination left missing or truncated by a put that
  died half way is put again, and the exe's original then comes from its
  verified backup.
- Any other unexpected content is refused, never overwritten.
- `--check` exits 2 with a banner while the share is in that damaged state.
