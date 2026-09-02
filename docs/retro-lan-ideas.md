# Retro LAN Ideas

A running list of things we want the retro LAN to do. This is a **wish list, not
a plan** - an entry here has not been designed, costed or promised. Nothing is
implemented until it appears in `CLAUDE.md`, a skill, or code.

Keep entries in the order they were added (newest at the bottom), each with the
date it was raised and a status. Do not delete an idea when it is done - mark it
`done` and point at what landed, so the list also records what we tried.

Status: `raised` | `exploring` | `done` | `dropped` (say why)

---

## 1. Per-user profiles - remember a player and their game settings

**Raised:** 2026-09-01 &nbsp;&nbsp; **Status:** raised

When someone introduces themselves to the retro chat, keep a profile for that
user, and save **all their game config files** so every machine can be
personalised to their taste - binds, sensitivity, resolution preference,
player name and colours, difficulty, audio levels.

The point is that a person sits down at whichever box is free and gets *their*
setup, instead of whatever the last player left behind.

Sketched only, not designed:

- A profile is keyed on the **person**, not the box. Today everything the fleet
  stores is per box (`fleetres.cfg`) or per title, and those axes stay - a
  person's preferences would be a third axis layered on top.
- The natural moment to apply one is the **launcher**, which already writes a
  fresh per-box config at every start. A per-user layer would be exec'd after
  the per-box one, so the person's taste beats the machine's defaults but the
  machine's hard limits still hold. It must not become a staged constant.
- Worth remembering: config files are also where **per-installation state**
  lives (CD keys, Westwood serials, an engine's detected machine spec). Those
  must NOT travel with a person - copying one machine's detected settings onto
  another is exactly the failure the library already has rules about.
- The engines all keep their settings in different places and formats
  (`autoexec.cfg`, `.ini`, registry, binary blobs), so "all their config files"
  is a per-title survey before it is a feature.

Open questions: how a person identifies themselves and how reliably; what
happens when two people want the same box; whether a profile is stored on the
dev host or on the NAS beside the library.
