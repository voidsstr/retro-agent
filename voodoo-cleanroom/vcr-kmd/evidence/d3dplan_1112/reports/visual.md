# Direct3D visual audit: every Direct3D frame from `.124` (2026-11-12)

Read-only review. Nothing was built, deployed or run on a box. Every PNG named
below was opened and looked at. Zoomed crops and pixel statistics were made in a
scratch directory. Statements that come from general DirectX or 3dfx knowledge,
not from a file I read, are marked **(inferred)**.

Path abbreviations:

| abbrev | path |
|---|---|
| `KMD` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/vcr-kmd` |
| `EV` | `KMD/evidence` |
| `S00` | `EV/d3dtune_1014/sweep_baseline/20261014_125245` |
| `S08` | `EV/d3dtune_1014/sweep_1108/20261108_004856` |
| `S08F` | `EV/d3dtune_1014/sweep_1108_fixed/20261108_020841` |
| `LAN` | `EV/lan_20260929_sweep` |
| `RES` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/scripts/benchmarks/results/v56k_lan_192.168.1.124` |
| `XEV` | `/home/voidsstr/.retro-fleet/evidence` |
| `PLAN` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/docs/v56k-benchmark-plan.md` |
| `VH5` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Displays/H5` |
| `LIB` | `/mnt/retro-share/Files/Games-Library` |

## 1. Summary

- **68 PNGs** were requested: 61 in `d3dtune_1014`, 7 in `lan_20260929_sweep`.
  `silicon/d3d32_step16` holds **no PNGs**, only logs and JSON; its title frames
  are in `RES/night/d3d32_titles/20260928_070504/`, and I reviewed those too. To
  answer the "in-game" question I also went through **157 more `.124` frames** of
  the same titles, in `RES/day/fbshots`, `RES/sweep*`, `RES/night` and `XEV`.
- **Only three Direct3D titles have an in-game frame at all:** Jedi Knight,
  Mysteries of the Sith and Thief II. All of them date from 2026-09-28, before the
  three 11-08 HAL fixes. Every frame of every other D3D title is a menu, intro
  video, loading screen, dialog or desktop (section 5).
- **Defects visible in the frames:**
  - **Fixed and confirmed by an after-frame:**
    - The half-pixel offset left the right column and bottom row unrendered (V-01).
    - DirectDraw-placed DXT textures off the 16-byte boundary drew colour noise (V-02).
  - **Fixed but proven only by a strip:** Rainbow Six's CreateDevice failure (V-03).
  - **Still open:**
    - A mode set leaves stale video memory on screen (V-04).
    - D3D titles go black or lose their backdrop when a Glide or OpenGL title ran
      earlier in the same boot (V-05, correlation only).
    - DX5/DX6 titles draw **point-sampled** (V-06). The likely cause is that the HAL
      ignores the legacy texture render states (`D3DRENDERSTATE_TEXTUREMAG/MIN/
      MAPBLEND/ADDRESS`, `D3DTSS_ADDRESS`) even though its DX5/6 caps advertise them.
    - Old-Dark-engine (DX6) foliage draws as **flat key-colour silhouettes** (V-07).
      The likely cause is that the caps advertise `D3DPTEXTURECAPS_TRANSPARENCY` but
      the 3D path has no colour key. This would also hit Thief Gold 1.37 and System
      Shock 2, which still run that engine.
    - UE2 titles run at **16 bpp** even though `Use16bit=False` and `D3D32` is armed (V-08).
  - **Minor or unconfirmed:** V-09 to V-13.
- **No frame shows or rules out** the HAL gaps already written down: lines and points
  skipped, MODULATE2X drawn as MODULATE, ALPHAREPLICATE, separate alpha blend
  factors (V-15). The frames that would show them do not exist.

## 2. How the frames were taken, and what that limits

- **`d3dtune_1014` frames are the agent's GDI `SCREENSHOT 0`**
  (`scripts/benchmarks/lan_sweep.py:175-181`).
  - `stats` in `sweep.json` is width, height, mean luma and stddev
    (`scripts/benchmarks/lan_check.py:186-194`).
  - The sweep's own docstring warns only about Glide/OpenGL titles
    (`lan_sweep.py:16-19`).
  - For a flipping D3D title, a GDI BitBlt reads whatever flip-chain buffer sits at
    the desktop offset. The miniport puts scan-out back there at every mode set
    (`KMD/miniport/vcrmp_hw.c:708`), and AvP's chain had its back buffer at 0x96000
    (`PLAN:207`). So after an odd number of flips, a GDI frame can be the **back
    buffer, possibly mid-render** **(inferred mechanism)**.
  - Single-colour or half-drawn transient frames (V-10, V-11) therefore prove nothing.
- **`LAN/*_fbshot.png` are `vcrctl fbshot`**, the frame being scanned out
  (`KMD/README.md:156`). The tool re-reads the start register on every line and
  follows flips, so a fast-moving cut-scene can come out as a mix of several frames.
  - `RES/day/fbshots/mots_new.png` shows exactly that kind of band structure, with
    edges at rows 25, 133, 268, 354 and 375.
- **`LAN/maxpayne_dt_menu_1280x960.png` is a GDI capture.** fbshot never finishes
  under Max Payne (`PLAN:599`).
- **The LAN PNGs are stored downscaled.** Four are 640x480 copies of 1280x960 frames,
  so a one-pixel defect cannot be seen in them.
- **Bit depth was read from the pixel statistics.** A 16 bpp frame has at most
  32/64/32 distinct levels, all of them 5:6:5 expansions. 32 bpp frames show about
  256 levels. 8 bpp palettised frames show at most 256 colours with values that are
  not 565-quantised.
- **Which driver build each frame set used:**

  | frame set | `vcrdd.dll` | evidence |
  |---|---|---|
  | `S00` (10-14) | pre-11-08 | `PLAN:183-246` |
  | `S08` (11-08 00:48) | b63ea6a5, before any 11-08 fix | the rollback copy, `PLAN:139-140`; `run.out` ends 01:45:00, commit 76d36e7 is 01:44:59 |
  | `sweep_1108/UT2003_menu_before_align_fix.png` | ef30672a, half-pixel fix only | `PLAN:138-149` |
  | `UT2003_menu_after_align_fix.png` and `S08F` (02:08-02:27) | 13256e46, half-pixel + alignment | `PLAN:158` |
  | `RainbowSix_intro_then_menu_z16in32.png` | **95678704 = the build deployed now** | `PLAN:170` |

  **The deployed build has never had a full sweep.** The re-sweep finished at 02:27,
  before commit be58b7d (02:41).

## 3. Frame by frame (the 68 requested PNGs)

"Informative" means the frame shows Direct3D triangle rendering that a HAL defect
could change.

### `S00`: 10-14 baseline (D3D32 and BigTex armed)

| file | shows | informative | defects |
|---|---|---|---|
| `Aliens_vs_Predator_30s.png`, `_60s.png` (pixel-identical) | 640x480x16. The old 1280x1024x32 desktop read at the new mode's depth and pitch: garbled green stripes, taskbar, icons. AvP stopped at a game-data assertion before it made any D3D device (`PLAN:203-216, 222-235`). | no | **V-04** |

### `S08`: 11-08 00:48, b63ea6a5, before the fixes

| file | shows | informative | defects |
|---|---|---|---|
| `Battlefield_1942_30s/60s.png` | Desktop 1280x1024x32, the BF1942 splash, the "Cannot locate the CD-ROM" dialog (SafeDisc 2.80). | no | none ours |
| `Battlefield_1942_Host_LAN_Server_30s/60s.png` | Desktop with the dedicated-server console. | no | none |
| `Battlefield_1942_Join_LAN_Game_30s/60s.png` | Desktop only; nothing started (it needs a partner). | no | none |
| `Halo_Combat_Evolved_30s.png` | Main menu 1280x960x32: planet, HALO logo, menu text, cursor. | menu quads | none visible |
| `Halo_Combat_Evolved_60s.png` | Nearly white (mean 241); a menu transition. | no | (V-10) |
| `Hidden_and_Dangerous_Deluxe_30s.png` | 1280x960x32. Profile-menu frame over a live **in-engine 3D backdrop** (manor, trees, lawn, bilinear-filtered); the logo watermark pattern belongs to the menu. | yes (backdrop) | none visible |
| `Hidden_and_Dangerous_Deluxe_60s.png` | Same, plus clean "Select game profile" and "create new" text. | yes | none |
| `Jedi_Knight_Dark_Forces_II_30s.png` | All black, one colour. | no (8 bpp, palette lost; `LIB/JediKnightDF2/README-FLEET.txt:42-47`) | none |
| `Jedi_Knight_Dark_Forces_II_60s.png` | Opening crawl in VGA-default colours, 9 colours. | no (8 bpp DirectDraw) | capture artefact |
| `Jedi_Knight_Mysteries_of_the_Sith_30s/60s.png` | Crawl: 101 colours, values not 565-quantised, so an 8 bpp surface. | no | none |
| `Red_Faction_30s.png` | THQ intro video. | no | none |
| `Red_Faction_60s.png` | Main menu 640x480x16. **Column 639 is white in all 480 rows and row 479 is white across all 640 pixels.** | yes | **V-01** |
| `Shogo_Mobile_Armor_Division_30s.png` | Menu backdrop 1280x960x16: blue waving cloth, SHOGO logo, kanji. No text yet. | yes | none visible |
| `Shogo_Mobile_Armor_Division_60s.png` | Backdrop with the MAIN MENU text, clean. | yes | none visible |
| `System_Shock_2_30s/60s.png` | Tri-Optimum menu 640x480x16. **Pixel-identical to the 09-29 `LAN/systemshock2_dt_fbshot.png`**, taken on a different HAL build (no xy_bias), so it is drawn by DirectDraw blits. | no (2D) | none |
| `Thief_Gold_30s/60s.png` | Intro video (Hammerite emblem; window scene). | no (FMV) | none |
| `Thief_II_The_Metal_Age_30s/60s.png` | NewDark D3D9 main menu 1280x960x32. **Column 1279 is black in 679 of 960 rows, and row 959 is one flat colour (10,14,10).** | yes | **V-01** |
| `Tom_Clancy_s_Rainbow_Six_30s/60s.png` | Black, with the RAINBOW SIX ERROR dialog. | failure | **V-03** |
| `Unreal_Tournament_2003_30s.png` | NVIDIA logo 640x480x16. **Coloured noise speckles fill the logo's background disc.** | yes | **V-02** |
| `Unreal_Tournament_2003_60s.png` | Main menu 640x480x16 with **coloured-noise text**. Column 639 is black (478/480) next to a white column 638, and row 479 is black. | yes | **V-02, V-01** |
| `Unreal_Tournament_2004_30s.png` | Black (loading). | no | none |
| `Unreal_Tournament_2004_60s.png` | Main menu 1280x960 at **16 bpp (565)**. The menu items are still sliding in from the right (animation). Column 1279 is black in 816/960 rows and row 959 in 754/1280 pixels. A bright line sits on row 958. | yes | **V-01, V-08, V-09** |
| `../UT2003_menu_before_align_fix.png` | 1280x960x16 on ef30672a; menu text is colour noise. | yes | **V-02** |
| `../UT2003_menu_after_align_fix.png` | Same menu on 13256e46; text clean. | yes | fixed |

### `S08F`: 11-08 02:08, 13256e46 (half-pixel + alignment)

| file | shows | informative | defects |
|---|---|---|---|
| `Battlefield_1942_30s/60s.png` (pixel-identical) | The SafeDisc dialog. | no | none ours |
| `Halo_Combat_Evolved_30s.png` | Main menu, correct; crisp text. | menu | none |
| `Halo_Combat_Evolved_60s.png` | **One colour, (255,255,255), with no logo, text or cursor.** | no | (V-10) |
| `Hidden_and_Dangerous_Deluxe_30s/60s.png` | Backdrop plus menu, as in `S08`. | yes | none |
| `Jedi_Knight_Dark_Forces_II_30s/60s.png` | Crawl, palette lost. | no | none |
| `Jedi_Knight_Mysteries_of_the_Sith_30s/60s.png` | Crawl, 8 bpp. | no | none |
| `Red_Faction_30s.png` | Intro video, dark. | no | none |
| `Red_Faction_60s.png` | Main menu; **no white pixels left in column 639 or row 479 (0/480, 0/640).** | yes | V-01 fixed |
| `Shogo_Mobile_Armor_Division_30s.png` | Backdrop, no text yet. | yes | none |
| `Shogo_Mobile_Armor_Division_60s.png` | **The backdrop cloth mesh drawn BLACK, wavy-edged, blue only at the borders, no menu text.** Mean luma 2. | ambiguous | **V-10** |
| `System_Shock_2_30s/60s.png` | Menu (2D). | no | none |
| `Thief_Gold_30s/60s.png` | Intro video ("Lest ye be struck down"; vase and window). | no | none |
| `Thief_II_The_Metal_Age_30s/60s.png` | Menu; the last column now matches its neighbour. | yes | V-01 fixed |
| `Tom_Clancy_s_Rainbow_Six_30s/60s.png` | Black, RAINBOW SIX ERROR. This run predates the 95678704 fix. | failure | V-03 |
| `Unreal_Tournament_2003_30s.png` | Epic / Digital Extremes / Atari logos 1280x960x16, centred, clean. | logo quads | none |
| `Unreal_Tournament_2003_60s.png` | **Black, with a thin yellow-green band at y 485-492 across the full width.** | ambiguous | **V-11** |
| `Unreal_Tournament_2004_30s.png` | Desktop with the UT2004 splash, before the mode set. | no | none |
| `Unreal_Tournament_2004_60s.png` | **In-engine 3D menu backdrop** (industrial bay, grid overlay, header band), no logo or items yet; 16 bpp. **Row 959 has mean 232; row 957 has 18.** | partly | **V-08, V-09** |
| `../RainbowSix_intro_then_menu_z16in32.png` | 960x240 strip: three 320x240 thumbnails of the intro and the main menu, on 95678704. | weak | V-03 (fixed) |

### `LAN`: 2026-09-29 (pre-11-08 HAL, D3D32 and BigTex armed)

| file | shows | informative | defects |
|---|---|---|---|
| `maxpayne_dt_menu_1280x960.png` (GDI, stored 640x480) | Max Payne D3D8 main menu. Its right edge is darker (edge statistic 8.0 against 1.8 one column in). The full-size `XEV/v56k-gametune_1001-fbshot_probe-20261004/fbab_maxpayne_probe.png` has column 1279 at mean luma 4.2, against 12.4 in column 1278 and 19.0 in column 1270, inside picture rows 110-845. | menu | **V-01** |
| `redfaction_fbshot.png` | All black (one colour): the scanned-out frame was black (`PLAN:602`). | failure | **V-05** |
| `systemshock2_dt_fbshot.png` | SS2 menu (2D). | no | none |
| `jediacademy_daemontools_fbshot.png`, `sof2_menu_fbshot.png` | id Tech 3 menus. **OpenGL, not D3D.** | out of scope | none |
| `serioussam_tfe_dt_fbshot.png`, `serioussam_tse_dt_fbshot.png` | Serious Engine, **OpenGL** on `.124` (`PLAN:93`). | out of scope | none |

## 4. Findings

### V-01: the right column and bottom row were left unrendered (half-pixel), fixed on 13256e46

- **Seen:**
  - `S08/Red_Faction_60s.png`: column 639 white 480/480, row 479 white 640/640.
  - `S08/Thief_II_The_Metal_Age_30s/60s.png`: column 1279 black in 679/960 rows; row 959 a single flat colour.
  - `S08/Unreal_Tournament_2003_60s.png`: column 639 black 478/480, row 479 black.
  - `S08/Unreal_Tournament_2004_60s.png`: column 1279 black 816/960, row 959 black 754/1280.
  - The pre-fix in-game frame `RES/day/fbshots/thief2_d3d9_armed_4.png`: column 1279 and row 959 entirely (0,0,0).
  - Max Payne, as above.
- **After the fix:** `S08F` Red Faction has 0/480 and 0/640, and the Thief II and UT2004
  edges carry content.
- **Cause, already fixed:** `xy_bias` = 0.5 (`KMD/display/vcrdd_3d.c:190-191, 304`).
  The vendor HAL has `pixelOffset` (`PLAN:134-135`). A full-screen quad drawn from
  -0.5 to W-0.5 by the D3D convention missed the last pixel centre on the Voodoo
  **(inferred geometry)**.
- **Still open:** Max Payne has no frame on the fixed driver.

### V-02: DXT textures off the 16-byte boundary drew colour noise (UT2003), fixed on 13256e46

- **Seen:**
  - `sweep_1108/UT2003_menu_before_align_fix.png`: noise text.
  - `S08/Unreal_Tournament_2003_60s.png`: noise text.
  - `S08/Unreal_Tournament_2003_30s.png`: multicoloured speckles in the NVIDIA logo disc around the character, at about x 215-355, y 160-300.
- **Fixed:** `UT2003_menu_after_align_fix.png`.
- **Cause:** textures placed by DirectDraw at offsets ending in 4
  (`sweep_1108/UT2003_combos_unaligned.txt`, the combine 20/22/24 `at` values).
  `mipchain_ext` now places every texture (`KMD/display/vcrdd_d3d.c:367, 441`).
- **Probably also V-12 (Halo squares).**

### V-03: Rainbow Six device creation failed (black frame plus error) with D3D32 armed

- **Seen in every armed sweep:**
  - `RES/night/d3d32_titles/20260928_070504`
  - `RES/sweep_armed3/20260929_041911`
  - the 10-01 baseline (`EV/gametune_1001/sweep_baseline/summary.md`)
  - `S08`
  - `S08F`
- **In the logs:** `EV/silicon/d3d32_step16/d3d32_end_vcrlog.txt:715` has
  "ContextCreate refused: 16 bpp target with a 32-bit Z".
- **Fixed on 95678704** (`vcr_rt_format` draws the pair as 16/16, `PLAN:162-175`).
  The only picture of the fix is the 320x240 strip.
- **Needed:** a sweep frame of R6's menu, and one in-game, on 95678704.

### V-04: a mode set leaves stale video memory on screen

- **Seen:**
  - `S00/Aliens_vs_Predator_30s/60s.png`: the old 32 bpp desktop shown as 640x480x16 garbage for the whole session.
  - `RES/sweep_armed3/20260929_041911/Unreal_Tournament_2004_30s.png`: the desktop, garbled, at 1280x960x16 before the game's first frame.
- **Code:**
  - `IOCTL_VIDEO_SET_CURRENT_MODE` masks off `VIDEO_MODE_NO_ZERO_MEMORY` (`KMD/miniport/vcrmp.c:674`).
  - `VcrHwSetMode` (`KMD/miniport/vcrmp_hw.c:652-718`) never clears the framebuffer.
  - NT's contract is to zero video memory unless that bit is set **(inferred, DDK)**.
- **Impact:** garbage before the first frame, and for the whole run of any title that
  stalls or only redraws part of the screen.
- **Fix candidate:** a 2D-engine fill of the new primary at mode set, only when the
  bit is clear. The hardware can do it (the 2D fill exists, `KMD/display/vcrdd_ddraw.c:450`).

### V-05: D3D titles go black or lose their backdrop after a Glide or OpenGL session in the same boot (correlation)

- **Seen:**
  - **Red Faction** draws **nothing**, even in the scanned-out frame
    (`LAN/redfaction_fbshot.png`; `RES/sweep_armed3/20260929_041911/Red_Faction_30s/60s.png`
    black). It ran right after GLQuake (OpenGL to our Glide) and RA2 (`LAN/sweep_part2.log`).
  - **Shogo** shows its menu text on **black, with no backdrop**:
    - `RES/sweep_armed3/.../Shogo_Mobile_Armor_Division_30s/60s.png`, after RTCW and Serious Sam;
    - `XEV/v56k-gametune_1001-sweep_baseline-20261001_192225/Shogo_Mobile_Armor_Division_30s.png`, after Serious Sam.
  - **H&D** in the 10-01 run, after Hexen II GL: the frame drawn on black at 30 s and
    all black at 60 s (`XEV/.../Hidden_and_Dangerous_Deluxe_30s/60s.png`).
  - **The same titles render fully in Glide-free boots:** `S08` and `S08F` Red Faction,
    Shogo 30 s and H&D. The per-title board check there is `glideprobe --noopen`
    (`lan_sweep.py:11-13`).
- **Code:**
  - The D3D path never writes `combineMode`, `chipMask` or `aaCtrl`
    (`KMD/include/vcr_3dregs.h:46-49`; no writes in `vcrdd_3d.c` or `vcrdd_d3d.c`).
  - It writes `stencilMode`/`stencilOp` only for 32 bpp targets
    (`KMD/include/vcr_3dseq.h:66-69`). That 16 bpp ignores stencil is "UNPROVEN"
    (`vcr_3dseq.h:44-46`).
  - "Stencil state and an extended colour combine outlive even a clean close"
    (`vcr_3dseq.h:84-85`; the list is at 87-127).
  - The reset that clears them, `Diag\Reset3D`, is default off and "has not run on
    silicon" (`KMD/display/vcrdd_2d.c:384-386`, gate at `:405`; `KMD/README.md:112`).
    The step16 boot log shows it off (`EV/silicon/d3d32_step16/step16_run.log`,
    "Glide 3D reset off").
- **Counter-evidence:**
  - On 09-29 H&D, the first title of sweep part 2, rendered fine, but a person used
    the box between the parts (`PLAN:586-587`).
  - On 09-28 23:46 H&D showed only its frame in a Glide-free boot. Its menu text was
    missing too, so that is more likely load timing.
- **Confidence: medium-low.** The A/B to settle it: cold boot, then Red Faction and
  Shogo, then GLQuake with a clean quit, then the same two again; repeat with
  `Reset3D`=1. If it is confirmed, the D3D context should write `combineMode`=0 and
  `stencilMode`=0 itself for every target **(inferred: cheap, no Glide dependency)**.

### V-06: DX5/DX6-era titles draw point-sampled textures (legacy filter states ignored)

- **Seen, in all 2026-09-28 fbshots:**
  - `RES/day/fbshots/jkdf2_d3d2.png`, `jkdf2_d3d3.png` (JK in game 1280x960x16) and
    `mots_game.png` (MotS in game) show hard-edged texel blocks on walls, faces and
    the floor, with no bilinear blending, under the 16 bpp dither.
  - `thief2_c2.png` (Thief II, original DX6 engine, 640x480x16) shows a blocky moon
    and ground.
  - **The same Thief II scene through NewDark D3D9 on the same card is bilinear:**
    compare the moon in `thief2_d3d9_armed_4.png`.
- **Code:**
  - The DX5/6 device description advertises the legacy filter and blend caps:
    `dwTextureFilterCaps` NEAREST/LINEAR/MIPNEAREST/LINEARMIPNEAREST,
    `dwTextureBlendCaps` DECAL/MODULATE/MODULATEALPHA/COPY/ADD
    (`KMD/display/vcrdd_d3d.c:2036-2043`).
  - `set_rs` only stores a render state (`vcrdd_d3d.c:1393-1405`). Filtering comes only
    from `D3DTSS_MAGFILTER/MINFILTER` (`:788-791`), whose defaults are POINT (`:1145-1146`).
  - `D3DRENDERSTATE_TEXTUREMAG`/`TEXTUREMIN`/`TEXTUREMAPBLEND`/`TEXTUREADDRESS` and
    `D3DTSS_ADDRESS` (12) are never read; the stage-state handler only stores them
    (`:1591-1611`).
- **Vendor:**
  - The vendor maps texMag to TS[0].magFilter (`VH5/D3RSTATE.C:2168-2212`) and texMin
    to min/mip filter (`:2226-2297`).
  - It maps texMapBlend to stage ops (`:2851-2910`), the legacy address state to U/V
    (`:2645-2655`), and `D3DTSS_ADDRESS` to U and V (`VH5/D7DP2.C:1602-1605`).
  - It notes TextureMapBlend is "still used for WINNT" (`VH5/D3INIT.C:124-131`).
- **Affected:** JK and MotS (DX5 execute buffers), and the original Dark engine:
  Thief Gold 1.37 and System Shock 2 have no NewDark files
  (`LIB/ThiefGold`, `LIB/SystemShock2` listings).
- **Risk, beyond the frames:** a MODULATEALPHA or DECALALPHA translucency drawn with
  the stage-0 default alpha (texture only), and CLAMP requests wrapping
  **(inferred, not seen)**.
- **Confidence: medium.** That these engines ask for bilinear filtering on 3D cards is
  **(inferred)**. Verify by logging `c->rs[17,18,21,3]` and `tss[12]` for one JK frame
  (the `log_combo` event can carry them), then map them as the vendor does.

### V-07: Dark engine (DX6) colour-keyed foliage draws as flat key-colour silhouettes

- **Seen:** `RES/day/fbshots/thief2_c2.png`. The trees and bushes are solid
  **(0,0,8) dark blue**:
  - tree region (250-330, 180-280): 3638 px of (0,0,8)
  - bush (580-620, 210-270): 2027 px of (0,0,8)
  - bush (365-400, 205-270): 2016 px of (0,0,8)

  The outline is tree- or bush-shaped, with no texture inside. The same objects are
  textured green in the D3D9 frame `thief2_d3d9_armed_4.png`.
- **Code:**
  - The DX5/6 caps advertise `D3DPTEXTURECAPS_TRANSPARENCY` (`KMD/display/vcrdd_d3d.c:2034-2035`).
  - The 3D path has no colour key at all: `V3D_CHROMAKEY`/`FZ_CHROMAKEY` are defined
    (`KMD/include/vcr_3dregs.h:32, 107`) and used nowhere.
  - Colour keys exist only for DirectDraw blits (`KMD/display/vcrdd_ddraw.c:434-456, 539-542`).
- **Vendor:** `colorKeyEnable` defaults to TRUE (`VH5/D3RSTATE.C:3029-3036, 3709`) and
  carries the texture's `dwColorSpaceLow/HighValue` (`VH5/D3GLOBAL.H:1464-1465`) to the
  chroma path (`VH5/D6MT.C:269, 603`).
- **Reading:** a leaf model's polygons with their key-coloured texels drawn opaque and
  then darkened by the night lighting would look exactly like this **(inferred)**.
  - Fog is ruled out: with no specular, the HAL disables vertex fog
    (`vcrdd_d3d.c:1358-1360`).
  - Darker overall lighting is ruled out: mean luma is 6.9 against 8.4 for the D3D9 frame.
- **Affected:** Thief Gold and System Shock 2 gameplay (same engine, never captured).
  Possibly other DX5/6 titles that colour-key **(inferred)**.
- **Confidence: medium.** Choices: either stop advertising TRANSPARENCY (the game then
  converts to an alpha format, which also needs V-06's alpha handling), or implement
  the key with the VSA-100 chroma key. The VSA-100 has one (vendor) — hardware supported.

### V-08: UE2 titles render at 16 bpp although they ask for 32

- **Seen:** every UT2003 and UT2004 frame is 565-quantised (`S08`, `S08F`), with an
  ordered-dither pattern visible on the menu backdrop's dark gradients.
- **The staged configs ask for 32 bpp:** `Use16bit=False`
  (`LIB/UT2004/System/UT2004.ini:329`, `LIB/UT2003/System/UT2003.ini:259`), and
  `D3D32` is armed.
- **The ring log shows UT2004's only context at 16 bpp:**
  `EV/silicon/d3d32_step16/d3d32_end_vcrlog.txt:906`, "ContextCreate 1: target 258000
  (1280x960 pitch 2560, 16 bpp 3D)". No refused 32 bpp attempt comes before it, so
  the choice happened above the driver.
- **Possible causes, unestablished:**
  - the D16-at-32 bpp `CheckDepthStencilMatch` lead;
  - a UE2 colour-depth default.
- **Next step:** read UT2004's own log for "16-bit" or "32-bit" and its device-format
  enumeration.
- **The other 32 bpp titles do get 32 bpp:** Halo, H&D and Thief II D3D9 are 32 bpp
  in the frames.
- **On a fresh install** (`D3D32` off) every D3D title is 16 bpp.

### V-09: UT2004, a one-pixel bright line on the screen's bottom row (minor, unconfirmed)

- **Seen:**
  - `S08F/Unreal_Tournament_2004_60s.png`: row 959 has mean 232.5, with 725 pixels of (231,231,231); row 958 is 93.9 and row 957 is 18.2.
  - Before the fix it sat one row higher: `S08/...60s.png` row 958 = 132.7, while row 959 was uncovered.
- **Reading:** the line moved with the half-pixel fix, so it is a real drawn texture
  edge. Bilinear sampling across a WRAP edge of a full-screen texture would produce
  it **(inferred)**.
- **Not known:** whether real D3D8 hardware shows it too. UT2004 is D3D8, so it sends
  ADDRESSU/V (13/14), which the HAL honours (`vcrdd_d3d.c:792-795`).
- **Confidence: low.** Compare against a reference capture before acting.

### V-10: transient single-colour or half-drawn GDI frames (evidence quality, not proven defects)

- **Halo:** `S08F/Halo_Combat_Evolved_60s.png` is one colour (255,255,255).
  - The plan calls it "its animated menu" (`PLAN:177-178`).
  - But the white phase of Halo's menu keeps the logo, menu text and cursor
    (`RES/day/fbshots/halo_libdeploy_15.png`, `halo_novideo_1.png`), so this is a
    flash or a buffer caught mid-frame.
- **Shogo:** `S08F/Shogo_Mobile_Armor_Division_60s.png` shows the backdrop mesh black,
  with no text, in a Glide-free boot.
  - It could be a back buffer captured between passes **(inferred)**.
  - It could be a real dropped-texture transient.
- **Confidence: low.** Recapture with fbshot `--probe`, several frames a second apart.

### V-11: UT2003 at 60 s is a thin horizontal band on black (ambiguous)

- **Seen:** `S08F/Unreal_Tournament_2003_60s.png`.
- **Two readings:**
  - an intro or menu "opening" transition **(inferred)**;
  - a collapsed full-screen quad.
- **Confidence: low.** The 00:48 run's 60 s frame showed the menu.

### V-12: Halo menu with flat opaque squares where soft sprites belong (pre-alignment fix)

- **Seen:** `RES/sweep_armed/20260928_234620/Halo_Combat_Evolved_30s.png`.
  - A 52x52 square at x 593-646, y 588-641 holds only 4 dithered colours around (38,53,36).
  - Two more hard-edged squares sit near (690,140) and (710,10) in the nebula.
  - No such squares appear in the 11-08 menu frames, but those catch a different
    animation moment.
- **Reading:** the pre-11-08 unaligned DXT read (V-02) would turn a small DXT3/5
  glow, alpha block included, into garbage. A missing texture or alpha would too
  **(inferred)**.
- **Confidence: low-medium.** Recapture Halo's planet-phase menu on 95678704.

### V-13: H&D full-screen noise with D3D32 armed on 09-28 07:05 (historical, regression watch)

- **Seen:** `RES/night/d3d32_titles/20260928_070504/Hidden_and_Dangerous_Deluxe_60s.png`
  is 1280x960 covered in colour noise with a periodic structure. The sweep called it
  PASS.
- **Clean on later builds:** `RES/sweep_armed3/20260929_041911`, `S08`, `S08F`.
- **Why keep it:** the PASS verdict came from luma statistics (mean 111), so a sweep
  verdict alone cannot catch this. A frame check is needed.

### V-14: Jedi Knight's and MotS's 3D view sits top-aligned: NOT a driver defect

- **Seen:** `jkdf2_d3d2/3.png` and `mots_game.png` draw the 3D view at x 180-1100,
  y 0-690, centred horizontally but at the top, with the gauges in the bottom corners.
- **Why it is not ours:**
  - The software 8 bpp path draws the same layout (`RES/day/fbshots/jkdf2_game.png`).
  - The view-size slider is mid-range (`mots_display.png`).
  - With pre-transformed vertices the HAL cannot move them anyway; it adds only
    `xy_bias` (`vcrdd_3d.c:190-191`).
- **Do not chase it.**

### V-15: known HAL gaps have no visual evidence either way

- **The gaps:**
  - lines and points are parsed and skipped (`vcrdd_d3d.c:1690-1715`);
  - MODULATE2X/4X fall to MODULATE (`:714`);
  - alpha blend is always ONE/ZERO (`:998`);
  - ALPHAREPLICATE is masked.
- **Why no frame can show them:** that needs in-game HUDs, particles, lightmaps and
  translucency, and those frames do not exist (V-16). Two gaps can be ruled out only
  partly:
  - **MODULATE2X:** the frames show no over-darkening. Thief II DX6 (mean luma 6.9) is
    no darker than D3D9 (8.4).
  - **Lines and points:** no HUD line or particle is visible in any frame.

## 5. Titles with NO in-game frame (V-16)

| title | best frame that exists | what it is |
|---|---|---|
| Aliens vs Predator | `S00/*` | the stale desktop; never reached a device |
| Battlefield 1942 (all 3 shortcuts) | `S08*/Battlefield_1942*` | SafeDisc dialog, server console, empty desktop |
| Halo | `S08F/Halo_Combat_Evolved_30s.png`, `RES/day/fbshots/halo_*` | main menu, Bink intros (`halo_libdeploy_13.png` is a video frame) |
| Hidden and Dangerous Deluxe | `S08F/Hidden_and_Dangerous_Deluxe_60s.png` | menu over an in-engine backdrop |
| Red Faction | `S08F/Red_Faction_60s.png` | main menu |
| Shogo | `S08/Shogo_Mobile_Armor_Division_60s.png` | main menu over the cloth backdrop |
| System Shock 2 | `S08F/System_Shock_2_30s.png` | 2D menu (DirectDraw, not D3D) |
| Thief Gold | `S08F/Thief_Gold_60s.png` | intro video only |
| Rainbow Six | `sweep_1108_fixed/RainbowSix_intro_then_menu_z16in32.png` | 320x240 intro and menu thumbnails |
| UT2003 | `sweep_1108/UT2003_menu_after_align_fix.png` | main menu |
| UT2004 | `S08F/Unreal_Tournament_2004_60s.png` | in-engine menu backdrop |
| Max Payne | `LAN/maxpayne_dt_menu_1280x960.png`, `XEV/.../fbab_maxpayne_probe.png` | main menu |

- **In-game frames exist only for:**
  - Jedi Knight DF2: `RES/day/fbshots/jkdf2_d3d2.png`, `jkdf2_d3d3.png`, 1280x960x16;
  - Mysteries of the Sith: `mots_game.png`;
  - Thief II: `thief2_c2.png` (old DX6) and `thief2_d3d9_armed_4.png` (NewDark D3D9).

  All are from 2026-09-28. They predate the half-pixel, alignment and R6 fixes, and
  no in-game frame exists on the deployed 95678704.
- **Never captured in Direct3D on `.124`:** the D3D renderers of the Glide/OpenGL
  titles (Unreal Gold, UT 436, Deus Ex, Descent 3, Turok 2, Carmageddon 2,
  Half-Life). The D3D frames that do exist are other hardware:
  - `EV/86box_v3/unreal_gold_d3d_vcrkmd_flyby*.png`, the 86Box Voodoo3 bed;
  - `EV/gametune_1001/descent3_timetest/*_123_d3d_*`, `.123`.

## 6. Captures the plan needs next (from this audit)

1. **One in-game frame per D3D title on 95678704**, taken with `fbshot --probe`
   (scan-out, no GDI back-buffer ambiguity). Use 2-3 frames a second apart, at full
   resolution, and do not downscale them.
2. **The A/B for V-05:** Glide title, then D3D title, in one boot; with and without
   `Reset3D`.
3. **A JK or Thief Gold DP2 render-state dump** to settle V-06 and V-07. Then add
   d3dprobe checks: legacy TEXTUREMAG LINEAR, a colour-keyed 1555/565 texture, and
   `D3DTSS_ADDRESS` CLAMP at a quad edge. d3dprobe is D3D8-only and D3D8 cannot send
   these states, so the checks need a DX6/DX7-interface mode.
4. **A frame check in the sweep verdict.** Luma statistics passed the H&D noise
   (V-13) and the stale desktop (V-04).
