---
name: xp-activation
description: Generate a Windows XP / Server 2003 Confirmation ID from an Installation ID, fully offline. Use when the user needs to activate XP, is stuck at the XP activation lockout screen, mentions "confirmation id", "installation id", phone/telephone activation, or a retro XP machine that can't reach Microsoft's (dead) activation servers.
---

# Windows XP Offline Activation

Microsoft retired the XP/2003 activation servers (internet **and** automated
phone). This skill computes the **Confirmation ID** from the **Installation ID**
locally — the same value Microsoft's phone system used to read back. It is not a
crack/loader; it patches nothing on the target machine and only works for IDs the
hardware itself produces. Intended for the user's own licensed retro fleet.

Tool lives at `scripts/xp-activation/` in this repo.

## Step 1 — Get the Installation ID from the XP machine

The user reads it off the **telephone activation** screen (9 groups of 6 digits =
54 digits). They can reach that screen even when locked out / not logged in:

- At the activation lockout: **Yes → "telephone a customer service representative"
  → pick any country** → the Installation ID is displayed.
- From a desktop instead: `Start → Run → %systemroot%\system32\oobe\msoobe.exe /a`.
- If normal-mode login is blocked by the lockout, **Safe Mode (F8)** still lets
  them log in to run the wizard.

This screen is also where they will type the Confirmation ID back in — no command
prompt or agent connection on the XP box is required.

## Step 2 — Build the generator (first run only)

```bash
cd scripts/xp-activation && ./build.sh    # produces ./xpcid (gcc/clang)
```

## Step 3 — Generate the Confirmation ID

Pass the Installation ID; spaces/dashes are ignored, so the 9 groups can be pasted
as-is:

```bash
./xpcid 001234 567890 123456 789012 345678 901234 567890 123456 789012
```

It prints the Confirmation ID as 7 groups of 6 digits. Give that to the user to
type into the wizard → **Next** → activated.

## Interpreting errors (relay these to the user)

- **"invalid check digit (mistyped group)"** — each group's 6th digit is a
  checksum; a digit was misread. Ask them to re-read that specific group. (This is
  a feature: it catches typos before producing a bad Confirmation ID.)
- **"invalid character"** — only digits/spaces/dashes allowed; a letter slipped in.
- **"unknown/unsupported version"** on a real ID — likely truncated; confirm all 54
  digits were entered.

## Notes

- The Installation ID is derived from hardware + product key and is **stable across
  reinstalls** (absent hardware changes), so the Confirmation ID can be reused on
  that machine.
- Supports both 45-digit (v5: XP/2003) and 41-digit (v4) layouts.
- For automating this across the retro fleet, the retro-agent could fetch the
  Installation ID and apply the Confirmation ID via the activation COM object, but
  the manual wizard path above is the reliable default and needs no agent.

See `scripts/xp-activation/README.md` for full provenance and details.
