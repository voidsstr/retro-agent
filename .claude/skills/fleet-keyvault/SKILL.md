---
name: fleet-keyvault
description: Read, add and rotate the retro fleet's secrets in Azure Key Vault nsc-secrets-kv - the Windows XP product key, per-title game CD keys and serials, host credentials. Use when you need a CD key or product key for a staged game or an unattended install, when a title asks for a serial, when staging a new game that has a key, when asked "where is the key for X", "put that key somewhere safe", "is anything secret committed", or before pasting any credential into a file.
---

# Fleet Key Vault — where the keys live, and when NOT to reach for one

Every secret this project depends on lives in **Azure Key Vault
`nsc-secrets-kv`**. The repo carries the *name* of a secret and the command that
fetches it, never the value. `scripts/pxe/SECRETS.md` is the register of what
each PXE-related secret is and how to recreate it; this skill is how you use the
vault day to day.

```bash
python3 scripts/fleet/keyvault.py list                  # every fleet-* secret
python3 scripts/fleet/keyvault.py show <name>           # metadata + tags, NOT the value
python3 scripts/fleet/keyvault.py get <name>            # the value, on stdout
```

---

## ⚠️ `aisleprompt-kv` IS NOT OURS — do not touch it

There is a second, unrelated vault called **`aisleprompt-kv`** belonging to the
AislePrompt project, and the user has a standing instruction that the specpicks
and aisleprompt systems are not to be changed. **Never read from it, write to
it, or reference it.** Everything the retro fleet needs is in `nsc-secrets-kv`
under the `fleet-*` prefix. `keyvault.py` refuses `aisleprompt-kv` in code
(`FORBIDDEN_VAULTS`), and `tests/python/test_keyvault.py` asserts that it does.

Note that `nsc-secrets-kv` *does* contain some non-fleet entries
(`file-aisleprompt-env`, `amazon-creators-*`, `file-development-*`). Those
belong to other systems. Stay inside `fleet-*`.

---

## THE RULE THAT MATTERS MOST: the vault is the SYSTEM OF RECORD, not a runtime dependency

A staged `install.reg` on the share carries the **literal** CD key. A `q3key`
file carries the literal key. That is correct and must stay that way:

* **A Windows `.reg` file cannot indirect through anything.** There is no syntax
  for "fetch this value from elsewhere"; `regedit /s` merges bytes.
* **A retro PC must never need the internet to start a game.** The fleet is a
  flat, isolated LAN of machines from 1998-2004. A launcher that phoned a cloud
  service would fail on every box the moment the WAN blinked, and would be
  undebuggable from a Windows 98 console.
* The agent's `GAMESYNC` copies a tree byte-for-byte. There is no hook in it
  where a secret could be substituted per box, and adding one would mean putting
  Azure credentials on the fleet — a far worse outcome than a CD key sitting in
  a file on a private LAN.

So the vault answers **"what was that key?"** — after a NAS rebuild, when
staging the title on a second library, when a box needs the key typed at a
dialog by hand, when the user asks. It is the **recovery path**, and the place
the value is described (what media it matches, what proved it works).

> **Do not "improve" a launcher, an `install.reg` or `GAMESYNC` into fetching a
> key at run time.** If a future session proposes it, this is the paragraph to
> point at. The literal in the staged tree is deliberate.

**Where the vault IS the runtime source:** host-side scripts on the dev host,
which already have `az`. `scripts/pxe/make-xp-source.sh` takes `PRODUCT_KEY`
from the vault at image-build time — that is the right shape, because the fetch
happens on the Linux host, and what reaches the fleet is a finished artifact.

---

## Naming convention

| Prefix | For |
|---|---|
| `fleet-gamekey-<title>` | a game's CD key / serial — one per product, lowercase, hyphenated |
| `fleet-winxp-*-key` | Windows product keys, **named after the media they were verified against** |
| `fleet-<service>-*` | host credentials, SSH keys, service env blobs |

Titles use the shortest unambiguous name: `fleet-gamekey-ut2004`,
`fleet-gamekey-quake3-team-arena`, `fleet-gamekey-half-life-goty`. Where one key
covers several products, say so in the `contentType` rather than creating an
alias — every GoldSrc title on a box (Half-Life, CS 1.6, Opposing Force, Blue
Shift) uses `fleet-gamekey-half-life-goty`, and only that one secret exists.

## Every secret carries a `contentType` and tags

The `contentType` is one sentence: **what it is, and what reads it.** Keep it
**under 255 characters** — Key Vault's limit, and it rejects a longer one with
`Property  has invalid value`, naming no property at all. `keyvault.py set`
checks this before it calls `az`.

Tags, on every `fleet-gamekey-*`:

| tag | what goes in it |
|---|---|
| `game` | the product's full retail name |
| `engine` | GoldSrc, id Tech 3, Unreal Engine 2, … — this predicts where the key lives |
| `path` | where in the staged library the literal sits, so the two can be reconciled |
| `source` | where it came from (`user-owned physical copy`, a specific ISO) |
| `verified` | **see below** |

### The `verified=` convention — the one that stops a myth spreading

`verified` records **what proved this key works**, not whether somebody believes
it does. Three legitimate values:

* `verified=pending` — stored, never presented to the engine. It is a record,
  not yet a fact.
* `verified=<date> <what proved it>` — e.g.
  `2026-08-29 two-box play on :27961`, or
  `2026-08-27 against en_windows_xp..._x14-80428.iso (Pid=76487000)`.
* `verified=REJECTED by <media/engine> on <date>` — a key that was tried and
  refused. **Keep it, do not delete it**, or somebody re-tries it in six months;
  `fleet-winxp-pro-sp3-product-key` exists purely to record a rejection.

A key that merely *exists* is not a key that *works*. A product key is matched
to its media's channel byte, and an unattended XP install has no way to click
past a refusal.

## Adding a new key

**Never pass a value on the command line** — argv reaches shell history, `ps`,
and this project's own transcripts. `keyvault.py set` only accepts a file or
stdin, and writes through a 0600 temp file it overwrites and unlinks.

```bash
umask 077
printf '%s' '<the key>' > /tmp/k.txt          # or extract it from the staged tree
python3 scripts/fleet/keyvault.py set fleet-gamekey-<title> --file /tmp/k.txt \
  --content-type "<what it is and what reads it, under 255 chars>" \
  --tag game="<retail name>" --tag engine="<engine>" \
  --tag path="Games-Library/<Title>/<where the literal lives>" \
  --tag source="user-owned physical copy" \
  --tag verified="pending"
shred -u /tmp/k.txt
```

Then **cross-check the vault against the tree** — a key stored wrong is worse
than one not stored, because it looks handled:

```bash
diff <(python3 scripts/fleet/keyvault.py get fleet-gamekey-<title>) \
     <(head -1 "/mnt/retro-share/Files/Games-Library/<Title>/<keyfile>")
```

## Staging a new title that has a CD key — do all three

1. Seed the literal in the staged tree (`install.reg`, or the engine's key file)
   so the title is **staged**, per the checklist in CLAUDE.md.
2. Store the value in the vault with the tags above.
3. Record **where the engine actually reads it from** in the `contentType`.
   This is not decoration — it differs per engine and each one cost time to
   find:

| engine | where the key lives | trap |
|---|---|---|
| GoldSrc | `HKCU\Software\Valve\Half-Life\Settings\Key` | stored with the **dashes stripped**; HKCU is the hive the engine reads |
| id Tech 3 (Q3, SoF2) | a plain-text `q3key` / `sof2key` **file**, key on line 1 | `cl_cdkey` is `CVAR_ROM` — it **cannot** be set from `autoexec.cfg` or the command line |
| Unreal Engine 2 (UT2004) | `System\cdkey` **and** the `CDKey` registry value | seed **both**; a box that has never run UT2004 has no `System\cdkey` |
| Westwood (RA2, TS) | `HKLM\SOFTWARE\Westwood\<game>\Serial` | **per-machine**, must be generated on the box — see below |

## THREE categories, not two — classify before you vault

Every key-shaped value you find is one of three things, and the expensive
mistake is folding the third into the first:

| category | what to do | example |
|---|---|---|
| **1. a real per-copy secret** | **vault it**, tagged, and keep the literal in the staged tree | the XP product key; `fleet-gamekey-ut2004` |
| **2. a deliberately-public fleet convention** | **document why**, do not vault | `retro-agent-secret`, `password`, `retroadmin` — see the table below |
| **3. per-installation machine-local state** | **leave it alone, and say so** | `HKLM\SOFTWARE\Westwood\<game>\Serial` |

### Category 3 in detail — vaulting it would break multiplayer

Some values must **differ between machines**. A vault entry would be exactly as
wrong as an `install.reg` entry, because both hand every box the *same* value.
Red Alert 2 and Yuri's Revenge refuse the second machine with *"There is already
a player with your serial# in that game"* when both read the same
`HKLM\SOFTWARE\Westwood\<game>\Serial`. **Tiberian Sun is the same lineage and
the same mechanism** — a two-box LAN test on 2026-08-30 produced two different
eleven-digit serials, which is the correct outcome, not a leak.

These are **generated on the box** by the launcher `.bat`, from the system
drive's volume serial, written only if absent. The reference implementation is
the top of `Games-Library/RedAlert2/Launch Red Alert 2.bat`; the whole class is
audited in `Games-Library/_patches/PER-BOX-VALUES.txt`. Do not invent a second
pattern, and do not "fix" a shared key that the engine does not police —
Quake III, Quake 2, Half-Life and CS 1.6 all share one key across the fleet with
two-box play verified.

### A vaulted key is NOT a staged title

The vault records a key; it says nothing about whether the game works. **Halo is
not staged**: `fleet-gamekey-halo-pc` carries `verified=pending` because a
2026-08-30 attempt could not get the key to validate and could not rule out its
own test harness as the cause. Leave that tag alone, and do not let any
documentation imply Halo is deployable.

## Deliberately NOT secret — say so rather than pretending

These are fleet-wide conventions on an isolated LAN with no WAN exposure.
Vaulting them would be security theatre and would break every script that
assumes the default:

| value | what it is | why it stays in the open |
|---|---|---|
| `retro-agent-secret` | the agent's shared auth secret | the default compiled into `agent/src/main.c`; ~20 scripts and both skills' libraries default to it. Rotating it is a **fleet-wide** operation — see the `security-posture` skill, not this one |
| `password` | the console account password on every box | required in cleartext by XP auto-login's `DefaultPassword`; CLAUDE.md's auto-login section documents it as the fleet convention, and a box that loses it needs a keyboard |
| `retroadmin`, `retro-vanilla`, `retro-noblood` | rcon / admin passwords on the game servers | LAN-only servers, `sv_lan 1`, no public listing |
| `user:password` in `agent/Makefile` | `SMB_CREDS` | a **placeholder** with `# EDIT:` beside it, not a real credential |

If you change your mind about any of these, change it *here* and in CLAUDE.md
together — a value that is documented as public in one place and vaulted in
another is worse than either choice.

## Known gap — the NAS credential is not vaulted

`/mnt/retro-share` is mounted from `//192.168.1.122/files` using
`/etc/cifs-retro-share.creds`, which is **root-only (0600)**. It is the one
credential the fleet depends on that is not in the vault, and it cannot be read
without `sudo`, which needs an interactive password on this host. **Ask the
user** before trying to capture it; the natural name would be
`fleet-nas-192-168-1-122-creds`, holding the `username=`/`password=` pair so the
mount can be recreated after a host rebuild. Do not guess at the value from the
fleet's `password` convention and store a guess — a wrong secret that looks
handled is worse than an absent one.

## Host / infrastructure credentials — Cloudflare (vaulted 2026-08-31)

**Cloudflare provides internet connectivity to this dev host**, so its
credentials are the one set here whose compromise reaches beyond the LAN. They
were transcribed from `~/Documents/cloudflare.odt` and vaulted; the project
name on the token page is `polished-bush-2355`.

| secret | what it is |
|---|---|
| `fleet-cloudflare-account-id` | Cloudflare account ID |
| `fleet-cloudflare-api-token` | API token — **scope unverified, treat as account-wide** until confirmed |
| `fleet-cloudflare-r2-access-key-id` | R2 (S3-compatible) access key ID |
| `fleet-cloudflare-r2-secret-access-key` | its paired secret |
| `fleet-cloudflare-r2-s3-endpoint` | the R2 S3 endpoint URL — not secret, stored beside its keys so the set stays together |

Two older Cloudflare entries already existed and are **not** duplicates of
these: `fleet-cloudflared-tgz` (the tunnel client binary) and
`file-cloudflared-cert-pem` (its origin certificate).

```bash
python3 scripts/fleet/keyvault.py get fleet-cloudflare-r2-secret-access-key
```

**These differ from every game key in this vault in one way that matters: they
are LIVE credentials to an internet-facing service, not a record of a value
that also sits in a staged file.** So the usual "the vault is the system of
record, not a runtime dependency" framing still applies — but the failure mode
if one leaks is not a re-typed CD key, it is someone else reaching this host.
Treat `~/Documents/cloudflare.odt` as the thing to remove once you are
confident the vault copies are good, and **never** echo one of these into a
transcript, a log, a commit, or the dashboard.

All five are tagged `verified=pending`: they were transcribed and stored, but
nothing has yet authenticated with them. Per the `verified=` convention above,
**move that tag only when something actually succeeds against Cloudflare** —
storing a value proves only that it was copied correctly.

## Halo: SEVEN keys, because the game allows one player each

`fleet-gamekey-halo-pc` and `fleet-gamekey-halo-pc-3` … `-8` are seven working
Halo: Combat Evolved keys. **Halo permits ONE simultaneous player per key** and
refuses the second machine with `Your CD Key is invalid` — the same wording as
a genuinely bad key — so the fleet needs one key per player, not one per
licence-holder. Assign them with `scripts/halo/assign_keys.py`, which refuses
to give two boxes the same key.

`fleet-gamekey-halo-pc-2` is tagged **REJECTED** and kept on purpose. It is
refused even as the sole player, tested twice on two boxes. Keeping a dead key
so nobody re-tries it is exactly what the `verified=` convention is for.

**Two things this taught that generalise:**

* **"It launches" is not verification.** All eight keys install and reach
  Halo's main menu identically; seven authenticate and one does not. The
  difference appears only at the server check, so a key can look completely
  fine and still be dead.
* **Check the format first — it is free.** Halo uses Microsoft's base-24
  alphabet `BCDFGHJKMPQRTVWXY2346789`, omitting every look-alike character
  (`A E I L N O S U Z 0 1 5`). A key containing one of those cannot be a Halo
  key, and no hardware test is needed to say so.

> **⚠️ AZURE TAG VALUES CAP AT 256 CHARACTERS**, and a longer one is dropped
> **silently** — `set-attributes` still reports success and the tag simply does
> not change. That happened here while recording the rejection above: the tag
> read `pending` afterwards and only a read-back caught it. This is the same
> shape as the documented 255-char `contentType` limit. **Always read the tag
> back.**

## Never commit a literal

`tests/python/test_no_committed_secrets.py` greps every tracked text file for
key-shaped literals (Microsoft 5x5, Unreal 4x5, Sierra 5x4, WON 4-5-4), for
private-key and connection-string markers, and — the catch-all — for the actual
values of every `fleet-gamekey-*` secret pulled live from the vault. It skips
**loudly** when `az` is unavailable, because a silently-passing guard is worse
than none.

**If you find a secret already in git history: report it and stop.** Do not
rewrite published history without asking the user first — several worktrees and
sibling repos track this branch. (Swept 2026-08-30: history is clean.)

## When `az` misbehaves

`keyvault.py` distinguishes four states because they have four different fixes,
and collapsing them is how an empty value ends up in a config file:

| exit | meaning | fix |
|---|---|---|
| 3 | `az` missing, or not logged in | `az login` |
| 4 | no such secret (or the value came back empty) | check the name with `list` |
| 5 | **access denied** — the secret may well exist | vault access policy / RBAC |
| 6 | vault unreachable, or an `az` failure we do not recognise | network, DNS, vault existence |

Never write `KEY=$(az keyvault secret show ... -o tsv)`. The assignment
swallows the error, `KEY` is empty, and the failure surfaces hours later on a
box as a dialog nobody is standing in front of. Use `keyvault.py get`, which
raises rather than returning a blank.
