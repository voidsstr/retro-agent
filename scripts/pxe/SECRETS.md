# Secrets for the retro PXE install server

Nothing sensitive is in this repo. Everything lives in Azure Key Vault
**`nsc-secrets-kv`**, and the scripts fetch it at run time. This file records
what each secret is, what needs it, and how to recreate it — so a host can be
rebuilt from nothing without archaeology.

> **This file is the register for the PXE / Windows-install secrets only.** The
> general convention — naming, the `verified=` tag, the `aisleprompt-kv`
> boundary, the per-engine table of where each game keeps its CD key, and the
> rule that the vault is a *system of record* and never a runtime dependency —
> lives in the **`fleet-keyvault` skill**
> (`.claude/skills/fleet-keyvault/SKILL.md`) and is summarised in `CLAUDE.md`.
> One source of truth: everything below is PXE-specific and is not repeated
> there.

Read one with the helper, which fails loudly instead of handing back a blank:

```bash
python3 scripts/fleet/keyvault.py get fleet-winxp-pro-sp3-x14-80428-key
python3 scripts/fleet/keyvault.py show fleet-winxp-pro-sp3-x14-80428-key   # metadata, no value
```

`az keyvault secret show --vault-name nsc-secrets-kv --name <name> --query value
-o tsv` does the same thing, but **never assign it to a variable** — when `az`
is not logged in the assignment swallows the error and you get an empty product
key in `winnt.sif`, which fails hours later at a dialog nobody is watching.

## Required to install Windows XP

| Secret | Used by | What it is |
|---|---|---|
| `fleet-winxp-pro-sp3-x14-80428-key` | `make-xp-source.sh` (as `PRODUCT_KEY`) | The product key **verified against** `en_windows_xp_professional_with_service_pack_3_x86_cd_x14-80428.iso`. Tagged with that filename and `verified=…`. |
| `fleet-winxp-pro-sp3-product-key` | nothing — **superseded** | An earlier key. Its `contentType` records that it was REJECTED by the x14-80428 media in 2026-08. Kept so nobody re-tries it. |

**The key and the media are a matched pair.** XP checks the key against the
channel byte in `I386/SETUPP.INI`; a key from a different channel is refused at
the "Your Product Key" dialog, and in an unattended install that is a hard stop
with no way to click past it. If you swap media, re-verify the key in a VM
before trusting it to hardware.

## Not secrets, but needed to rebuild

| Thing | Where it comes from |
|---|---|
| Retail XP SP3 media | The fleet NAS, `Files/OS/en_windows_xp_professional_with_service_pack_3_x86_cd_x14-80428.iso`. Not redistributable through this repo. |
| DriverPacks (~1.4 GB) | `DriverPacks-XP-32.7z`. On the NAS under `Files/Drivers/`; also mirrored on archive.org as `driver-packs-xp-32.7z`. Pass to `bootstrap-host.sh` as `PACKS_ARCHIVE=`. |
| The game library | `\\192.168.1.122\files\Files\Games-Library` — pulled by the agent's GAMESYNC after install, not part of the image. |

## Other `fleet-*` secrets in the vault

These belong to other fleet systems and are **not** used by the PXE server.
Listed so a rebuild does not go hunting for a relationship that is not there:
`fleet-backup-manifest`, `fleet-migration-manifest`, `fleet-secrets-env`,
`fleet-ssh-id-ed25519*`, `fleet-cloudflared-tgz`, `fleet-responder-tgz`,
`fleet-seo-tgz`, `fleet-claude-pool-*`.

The **`fleet-gamekey-*`** entries are the staged library's CD keys and serials
(Half-Life/GoldSrc, Quake III, SoF2, UT2004, Halo). They are nothing to do with
installing Windows — they are seeded into the staged trees on the share and are
documented in the `fleet-keyvault` skill. `python3 scripts/fleet/keyvault.py
list` shows the current set.

> The `fleet-claude-pool-*` entries have a trap of their own recorded elsewhere:
> never probe a pool profile with `claude --print`, which blanks the stored
> tokens if the refresh fails.

## Rotating the XP key

```bash
umask 077
printf '%s' 'XXXXX-XXXXX-XXXXX-XXXXX-XXXXX' > /tmp/k.txt
python3 scripts/fleet/keyvault.py set fleet-winxp-pro-sp3-<media-id>-key \
  --file /tmp/k.txt \
  --content-type "XP Pro SP3 key - verify against <iso name> before use" \
  --tag image="<iso name>" --tag edition="Windows XP Professional SP3 x86 English" \
  --tag source="<where it came from>" --tag verified="pending"
shred -u /tmp/k.txt
```

(The helper passes the value through a 0600 temp file, never argv, and refuses a
`--content-type` over 255 chars — Key Vault's limit, which it otherwise rejects
with an unhelpful `Property  has invalid value`.)

Then rebuild the payload and **prove it in a VM before touching hardware** —
`scripts/pxe/` has everything needed to boot one. Update the `verified` tag once
setup gets past the key dialog.
