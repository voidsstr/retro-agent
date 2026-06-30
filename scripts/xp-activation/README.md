# Offline Windows XP Activation (Confirmation ID generator)

Generates a valid **Confirmation ID** from the **Installation ID** shown on the
Windows XP / Server 2003 *telephone activation* screen — completely offline.

This is needed because Microsoft retired the XP activation servers (both internet
and the automated phone system). A licensed XP machine that falls out of
activation (reinstall, hardware change, expired grace period) can no longer reach
Microsoft to get a Confirmation ID. This tool reproduces exactly what Microsoft's
phone operator/IVR used to return — it is not a crack or a loader and patches
nothing on the target machine.

## Build

```bash
cd scripts/xp-activation
./build.sh          # produces ./xpcid  (needs gcc or clang)
```

## Use

1. On the XP box, get to the Installation ID:
   - At the activation lockout, choose **Yes → telephone activation**, or
   - From a desktop: `Start → Run → %systemroot%\system32\oobe\msoobe.exe /a`
     → *"Yes, I want to telephone..."* → pick any country.
   - The wizard shows the **Installation ID**: 9 groups of 6 digits (54 total).
2. Generate the Confirmation ID:

   ```bash
   ./xpcid 001234 567890 ...   # paste the 9 groups; spaces/dashes ignored
   # -> 7 groups of 6 digits = your Confirmation ID
   ```
3. Type the Confirmation ID into the wizard's entry boxes → **Next** → activated.

The Installation ID is derived from hardware + product key, so it's **stable
across reinstalls** as long as hardware doesn't change — the same Confirmation ID
keeps working on that machine.

## Notes / troubleshooting

- **"invalid check digit (mistyped group)"** — every 6-digit group's last digit is
  a checksum, so this means a digit was misread off the screen. Re-read that group.
  This catches typos *before* you get a bad Confirmation ID.
- **"unknown/unsupported version"** on a *real* ID — double-check you entered all 54
  digits; a truncated/garbled ID can decode to a bad version field.
- Supports both the 45-digit (v5, XP/2003) and 41-digit (v4) installation ID layouts.

## Files

- `xpcid.c`   — portable CLI wrapper (reads ID from argv, prints Confirmation ID).
- `core.inc`  — the verbatim activation algorithm core (see header for provenance).
- `build.sh`  — one-line build.

## Provenance

Algorithm core is extracted verbatim from
[Endermanch/XPConfirmationIDKeygen](https://github.com/Endermanch/XPConfirmationIDKeygen)
(the reverse-engineered XP phone-activation algorithm; hyperelliptic-curve based).
Only the Windows GUI/COM layer was removed and `wchar_t` swapped to `char` so it
builds as a plain Linux CLI.
