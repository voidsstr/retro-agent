# New-host test: reach whitebeast's retro_agent (2026-08-24)

FOR THE CLAUDE SESSION ON THE NEW HOST: the operator just opened whitebeast's
firewall for the retro agent (scripts/enable-agent-lan-access.ps1, run
2026-08-24). Your job: verify this host can drive whitebeast's agent, then
keep these facts for future work.

## Facts you need

- whitebeast (Windows 11 + WSL2) LAN IP: 192.168.1.82
- retro_agent: C:\RETRO_AGENT\retro_agent.exe, binds 0.0.0.0
  - TCP 9898 = protocol port, auth secret: retro-agent-secret
  - UDP 9899 = discovery
- Firewall allows: LocalSubnet (192.168.1.0/24) + 100.64.0.0/10 (Tailscale).
  If this host is NOT on 192.168.1.x, use whitebeast's Tailscale IP instead.
- Protocol client: nsc-assistant repo, shared/retro_protocol.py
  (RetroConnection). Same client the fleet uses for 192.168.1.123/.133/.143/.145.
- Game servers on whitebeast (launched via the agent, interactive session):
  CS 1.6 vanilla UDP 27018, CS 1.6 no-blood UDP 27017, UT99 UDP 7777
  (query 7778).
- IMPORTANT: whitebeast CANNOT probe its own LAN IP (hairpin false negative).
  From THIS host, probing 192.168.1.82 is valid and expected to work.

## Test ladder (run in order)

1. Port reachability:
       nc -vz 192.168.1.82 9898                      # Linux/WSL
       Test-NetConnection 192.168.1.82 -Port 9898    # PowerShell
   Expect: succeeded. Fail -> wrong subnet (try Tailscale IP) or agent down
   on whitebeast (operator starts it by double-clicking the exe there).

2. Protocol handshake + harmless command (proves auth + desktop bridge):
       cd ~/development/nsc-assistant && python3 - <<'PY'
       import asyncio, sys
       sys.path.insert(0, ".")
       from shared.retro_protocol import RetroConnection
       async def main():
           c = RetroConnection("192.168.1.82", 9898)
           await c.connect("retro-agent-secret", timeout=10.0)
           resp = await c.send_command("WINLIST")
           print("OK:", str(resp)[:200])
       asyncio.run(main())
       PY
   WINLIST is read-only (lists desktop windows). PASS = full chain works.

3. Game servers visible from LAN (optional):
   A2S query to 192.168.1.82:27018 / :27017, UT99 \status\ to :7778.
   These have their own firewall rules from start-game-servers.ps1.

## Rules that carry over to this host

- hlds.exe / UCC.exe on whitebeast may ONLY be launched via the agent's
  EXEC (cmd /c start "name" /min /D <dir> <exe> <args>). Never WSL interop
  Start-Process, never schtasks: both create unkillable zombies that pin
  UDP ports. Exact launch patterns: scripts/game-servers/README.md.
- Agent unreachable -> operator action on whitebeast (start the exe in the
  interactive session). No safe remote restart.

## Everything else this host needs

Secrets + full fleet handoff live in Azure Key Vault nsc-secrets-kv
(rg nsc-apps). Pull order:
  1. az login
  2. az keyvault secret show --vault-name nsc-secrets-kv \
       --name fleet-migration-manifest --query value -o tsv   # READ FIRST
  3. clone reusable-agents; bash install/recover-credentials.sh restore
Docs: reusable-agents/docs/keep-the-lights-on.md (sections: secrets backup +
host migration pull; whitebeast retro-agent).
