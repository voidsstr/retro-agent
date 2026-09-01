# Tom Clancy's Rainbow Six (1998) — staged 2026-09-01

The library tree is `Games-Library/RainbowSix/` (6,213 files, 376,845,268 B).
These are the four fleet files added on top of the vendor build; the tree
itself is the vendor's, unmodified.

## What the media is, and how that was established

**An official GOG.com build**, not a retail-CD install and not a rip. Proven
two ways rather than asserted:

* `goggame-1207658752.hashdb` is GOG's own manifest — **6,201 of 6,201 files
  match it byte for byte.**
* The remaining 12 files are GOG's own metadata and uninstaller;
  `unins000.exe` carries a DigiCert-chained **GOG Sp. z o.o. Authenticode
  signature** that verifies against the file's actual PE hash.
* Corroboration the manifest cannot fake: the 6,201 covered files total
  373,565,219 B = 373.6 MB, matching gogdb's listed depot size for build
  51031613339520337 (product 1207658752, v001.04) exactly; and
  `RainbowSix.exe` still carries Red Storm's own build path
  `C:\develop\sherman\release` at VA 0x89873C.

## THE CD CHECK IS STILL IN THE BINARY — that is the point

It was **not** patched out, which is exactly what a cracked executable would
not look like:

    0x40B1CB  call [GetVolumeInformationA] ; cmp eax,1 ; inline strcmp of label
    0x40B215  MessageBeep / MessageBoxA ; cmp eax,2 (IDCANCEL)
    0x40B236  je 0x40B1A3  -> Retry/Cancel loop, re-reads the drive

and its message text ships as
`data/text/interface/english/dialogueCD.txt` — *"Please insert the Rainbow Six
CD in drive "*. The game runs disc-free because the **vendor sets a registry
value**, not because anyone touched the code. Read `runs without the disc` as
evidence of a crack here and you would condemn provably pristine media.

The gate, traced from the binary: `InstallType` is compared at 0x40A17A and an
equal result **skips** the check. GOG writes `InstallType="Full"` and
`CDPath="C:\"`.

## WHY `install.reg` REPLACES GOG's `regs.cmd` — the one real defect

`regs.cmd` ends **all 40** of its `REG ADD` lines with `/reg:32`. **Windows XP's
`reg.exe` has no `/reg:32`** — it arrived with Vista. So on XP every line fails
and not one value is written, while the errors scroll past.

That is not cosmetic. **33 of the 34 asset-path defaults compiled into
`RainbowSix.exe` point at `\data2\`** — the CD — and only `\data\journals`
defaults to disk. With no registry values the game finds essentially nothing.

`install.reg` is generated from `regs.cmd` itself (so the two cannot drift),
rewrites the paths to `C:\Games\RainbowSix`, and is applied by GAMESYNC with
`regedit /s`, which avoids `reg.exe` entirely and behaves identically on XP and
Win7. It also seeds `App Paths\RainbowSix.exe`, which `regs.cmd` *reads back*
to build its own `{app}` and which a file-copy deploy would never have.

**Never run the shipped `regs.cmd` on a fleet box.** It stays in the tree only
because it is part of the vendor build.

## Launcher

No FLEETRES block, deliberately. The whole image contains exactly two
switch-shaped tokens, `-server` and `-client`; there is **no** resolution
switch, no windowed switch and no dedicated-server switch. Fullscreen and
resolution are registry values, set in `install.reg`.

## Gate floors

MSVC 5.10, built Nov 1998 — which **predates SSE** (Feb 1999), so an SSE floor
is impossible; measured 0 movaps, 0 SSE2, 0 CMOV under a real disassembly.
MMX is *detected* (one CPUID site at 0x47405C reading CPUID.1:EDX bit 23) and
used as an optional fast path, never required. Engine ships a software
renderer; the 3D path is Direct3D through DirectDraw and there is **no Glide
path at all**, so the Voodoo boxes gain nothing here. Every fleet box clears
the floor.

## No CD key

1998 retail had no online activation and GOG strips serials. Nothing to supply,
nothing vaulted. There is also **no Rainbow Six 1 disc image on the share** —
the four ISOs in `Games/Inbox/Rogue Spear Collection/` are a different game
(volume labels ROGUESPR, URBANOPS, BLACKTHORN, COVERTOPS).
