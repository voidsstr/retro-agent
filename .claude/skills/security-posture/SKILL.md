---
name: security-posture
description: Interactive security self-assessment and authorized penetration test of the retro agent and the fleet it manages, with ranked hardening recommendations the user can reply to and apply. Use when the user asks to review the agent's security, run a pen test, check exposure, harden the fleet, rotate the secret, or "is this thing safe?". Scope is the user's OWN licensed hardware on the isolated LAN only.
---

# Security Posture — self-assessment, authorized pen test, and hardening

This skill runs an **authorized** security review of the retro agent and the
fleet it manages. It exists so the project can demonstrate an active, ongoing
security posture: measure exposure, test our own controls, and apply hardening.

**Authorization & scope (non-negotiable):**
- Only assess machines the user owns/administers on their **own isolated LAN**.
  This is self-assessment, not offensive work against third parties.
- Default to **read-only** probes. Anything that changes state (rotating the
  secret, restricting a bind address, disabling a service) is proposed first and
  applied only after the user picks it.
- Never exfiltrate data off the LAN. Findings stay local; the summary goes back
  to the user on the retro console.

## How to run it (interactive, from the retro chat)

1. Discover in-scope agents with `retro_list_machines` (or
   `client/retro_discovery.discover_retro_pcs`). Confirm the list with the user
   before probing anything.
2. Run the **assessment checklist** below (read-only). Collect findings.
3. Print a ranked, **numbered** findings list as a plain-ASCII summary (the retro
   console is 16-color ASCII — no Unicode). Each finding names the risk, the
   affected machine(s), and a one-line fix.
4. Invite the user to **reply with the numbers** they want to act on. Apply only
   those, one at a time, echoing what changed. Re-check after applying.

Keep it conversational: the user can reply "explain 3", "apply 1 and 4", or
"skip reboots" and you act on exactly that.

## Assessment checklist (read-only probes)

Run these against each in-scope agent and record pass/fail + detail.

1. **Authentication strength.**
   - Is the agent still using the well-known default secret
     (`retro-agent-secret`)? Test by attempting an AUTH with the default; if it
     connects, flag HIGH.
   - Is the secret short/guessable? Recommend a long random secret per
     deployment.
2. **Network exposure.**
   - The agent binds all interfaces. Confirm it is reachable **only** on the
     trusted LAN — check for a route/NAT rule that exposes port 9898 (or the
     discovery UDP 9899) to the internet. Any WAN reachability is CRITICAL.
   - `LICSTATUS`, `SYSINFO` are fine read-only fingerprints to confirm identity.
3. **Transport confidentiality.**
   - The optional XOR layer (`AUTH_ENC`) is scrambling, **not** a security
     boundary. If the link ever leaves a trusted LAN, recommend tunneling over
     TLS/SSH (stunnel / SSH port-forward) instead of relying on it.
4. **Command-surface exposure.**
   - The agent can run arbitrary commands, read/write files and registry, and
     capture the screen. Confirm only trusted operators/hosts can reach it
     (firewall allowlist of the dev box IP). Recommend an ACL if the LAN is
     shared.
5. **Auto-update integrity.**
   - Updates are pulled from an SMB share by version compare. Confirm the share
     is write-restricted to trusted admins (a writable share = fleet-wide code
     execution). Recommend hash/signature verification of the pulled binary.
6. **Brain autonomy controls.**
   - Confirm the chat brain's fleet-safety guardrail is active
     (`RETRO_BRAIN_REQUIRE_CONFIRM` not disabled) so destructive agent commands
     need explicit confirmation. Review `allowed_tools`.
7. **Patch/version drift.**
   - Collect `agent_version` (SYSINFO) across the fleet; flag machines behind the
     current build (missing fixes/guardrails).

## Ranked recommendations (offer these as reply-able items)

Present whichever apply, HIGH→LOW, each as a numbered item the user can select:

- **Rotate the agent secret** to a long random value; update the daemon/brain
  env (`RETRO_AGENT_SECRET`) and each agent's launch args. (Fixes checklist 1.)
- **Restrict reachability**: firewall port 9898/9899 to the dev-box IP only, or
  put the fleet on an isolated VLAN. (Fixes 2, 4.)
- **Tunnel off-LAN traffic** over SSH/TLS if any machine is remote. (Fixes 3.)
- **Lock down the update share** to admin-write only; add binary hash
  verification. (Fixes 5.)
- **Keep the brain guardrail on** and review the allowed toolset. (Fixes 6.)
- **Roll the fleet forward** to the current agent build. (Fixes 7.)

## Applying a fix (only what the user picked)

- Secret rotation, firewall/ACL changes, service config: make the change, then
  re-run the relevant checklist item to confirm it took.
- Anything that reboots or interrupts a machine goes through the normal fleet
  rule — **ask before every reboot**; via the brain, destructive commands need
  `confirm=true`.
- After a round, reprint the summary with items marked done, and ask if they want
  another pass.

## Output

A plain-ASCII block suitable for the retro console, e.g.:

```
SECURITY POSTURE - fleet scan (3 agents)
  [HIGH] 1  .124  default secret accepted  -> rotate secret
  [MED ] 2  .143  agent v1.5.1 (behind 1.6.0) -> update agent
  [OK  ]    .124  not reachable from WAN
Reply with numbers to fix (e.g. "1"), or "explain N".
```
