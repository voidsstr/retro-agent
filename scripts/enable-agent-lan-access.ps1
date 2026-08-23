# enable-agent-lan-access.ps1 — open Windows Firewall so OTHER hosts can
# reach whitebeast's retro_agent (C:\RETRO_AGENT\retro_agent.exe).
# RUN ELEVATED once (self-elevates). Idempotent.
#
# Why: the agent binds 0.0.0.0:9898 (TCP protocol) + 0.0.0.0:9899 (UDP
# discovery), but Windows Firewall has no inbound allow rule, so only WSL
# on this machine (via the vEthernet adapter's built-in allowance) can
# connect. The new fleet host on the LAN — or over Tailscale — gets
# silently refused until these rules exist.
#
# Scope: LocalSubnet (192.168.1.0/24 fleet + new host) plus the Tailscale
# CGNAT range 100.64.0.0/10 so a tailnet host can drive the agent too.
#
# NO AGENT RESTART NEEDED — it already listens on 0.0.0.0; firewall rules
# apply immediately. (If the agent is ever down, start it by double-clicking
# C:\RETRO_AGENT\retro_agent.exe in the interactive session — never via WSL
# interop or schtasks.)

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
  exit
}

Write-Host "== [1/2] Firewall rules for retro_agent =="
$rules = @(
  @{ Name = "Retro Agent protocol (TCP 9898)";  Proto = "TCP"; Port = 9898 },
  @{ Name = "Retro Agent discovery (UDP 9899)"; Proto = "UDP"; Port = 9899 }
)
foreach ($r in $rules) {
  if (Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue) {
    Write-Host "  ok (exists): $($r.Name)"
  } else {
    netsh advfirewall firewall add rule name="$($r.Name)" dir=in action=allow `
      protocol=$($r.Proto) localport=$($r.Port) remoteip="LocalSubnet,100.64.0.0/10" profile=any | Out-Null
    Write-Host "  ADDED: $($r.Name)"
  }
}

Write-Host "== [2/2] Verify agent is listening =="
$tcp = Get-NetTCPConnection -LocalPort 9898 -State Listen -ErrorAction SilentlyContinue
if ($tcp) {
  $p = Get-Process -Id $tcp[0].OwningProcess -ErrorAction SilentlyContinue
  Write-Host "  agent UP: pid $($tcp[0].OwningProcess) ($($p.ProcessName)) on 0.0.0.0:9898"
} else {
  Write-Host "  agent NOT listening — start C:\RETRO_AGENT\retro_agent.exe (double-click, interactive session)"
}
Write-Host ""
Write-Host "Done. From the new host test:  nc -vz 192.168.1.82 9898   (or Tailscale IP)"
Read-Host "Press Enter to close"
