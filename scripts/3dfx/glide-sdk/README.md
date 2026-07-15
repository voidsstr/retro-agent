# Vendored Glide3 SDK (open-source)

Headers and import libraries for building against the open Glide3 API, used by
`../gfxbench` and `../d3dhal`. These are **inputs**, checked in so those
programs build without re-cloning/re-building Glide.

- `include/` — public Glide3 headers (`glide.h`, `g3ext.h`, `glidesys.h`,
  `glideutl.h`, `sst1vid.h`, `3dfx.h`) from the open Glide release.
- `lib/libglide3x_h5.dll.a` — import lib for the Voodoo4/5 (VSA-100) `glide3x.dll`
- `lib/libglide3x_h3.dll.a` — import lib for the Voodoo3 `glide3x.dll`

## Provenance / license

Vendored from [sezero/glide](https://github.com/sezero/glide) (the maintained
fork of 3dfx's SourceForge CVS release), licensed under the **3dfx Glide Source
Code General Public License** — 3dfx's genuine 2000 open-source release, not the
2003 leak. Regenerate with `../build-glide.sh` (it produces the DLLs; the import
libs and headers come from the same tree).
