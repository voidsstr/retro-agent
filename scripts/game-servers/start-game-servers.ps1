# start-game-servers.ps1 v2 — whitebeast game servers (CS 1.6 x2 + UT99).
# RUN ELEVATED once after boot (self-elevates). Idempotent.
#
# 2026-08-20 rebuild: F: died, everything now lives at C:\gameservers.
# Ports: vanilla 27018, noblood 27017 (27015/27016/27019 are pinned by
# unkillable hlds zombies until the next reboot — stay on these ports even
# after a reboot to avoid favorites churn). UT99: 7777 game / 7778 query /
# 8777 LAN beacon.
#
# LAUNCH QUIRK: hlds/UCC must start in a real interactive session with a
# working directory. Elevated PowerShell run by the logged-in user is fine.
# Launching from WSL interop or schtasks produces zombie processes that pin
# their UDP port until reboot — never do that.

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
  exit
}

Write-Host "== [1/4] Firewall rules (scoped to LocalSubnet) =="
if (-not (Get-NetFirewallRule -DisplayName 'CS 1.6 LAN servers (UDP)' -EA SilentlyContinue)) {
  netsh advfirewall firewall add rule name="CS 1.6 LAN servers (UDP)" dir=in action=allow protocol=UDP localport=27015-27020 remoteip=LocalSubnet profile=any | Out-Null
  Write-Host "  CS rule added"
} else { Write-Host "  CS rule present" }
if (-not (Get-NetFirewallRule -DisplayName 'UT99 LAN server (UDP)' -EA SilentlyContinue)) {
  netsh advfirewall firewall add rule name="UT99 LAN server (UDP)" dir=in action=allow protocol=UDP localport=7777-7778,8777 remoteip=LocalSubnet profile=any | Out-Null
  Write-Host "  UT99 rule added"
} else { Write-Host "  UT99 rule present" }

Write-Host "== [2/4] Start servers (skip if that port already answers) =="
function Test-A2S([int]$port) {
  try {
    $udp = New-Object Net.Sockets.UdpClient; $udp.Client.ReceiveTimeout = 3000
    $udp.Connect('127.0.0.1', $port)
    $q = [byte[]](0xFF,0xFF,0xFF,0xFF,0x54) + [Text.Encoding]::ASCII.GetBytes("Source Engine Query") + 0x00
    [void]$udp.Send($q, $q.Length); $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
    [void]$udp.Receive([ref]$ep); $udp.Close(); return $true
  } catch { return $false }
}
$cs = @(
  @{ Name='cs16-vanilla'; Dir='C:\gameservers\cs16-vanilla'; Port=27018 },
  @{ Name='cs16-noblood'; Dir='C:\gameservers\cs16-noblood'; Port=27017 }
)
foreach ($s in $cs) {
  if (Test-A2S $s.Port) { Write-Host "  $($s.Name) already answering on $($s.Port)"; continue }
  Start-Process -FilePath "$($s.Dir)\hlds.exe" -WorkingDirectory $s.Dir `
    -ArgumentList '-console','-condebug','-game','cstrike','-port',"$($s.Port)",'-maxplayers','16','+sv_lan','1','+log','on','+map','de_dust2' `
    -WindowStyle Minimized
  Write-Host "  started $($s.Name) on UDP $($s.Port)"
}
$utUp = $false
try {
  $udp = New-Object Net.Sockets.UdpClient; $udp.Client.ReceiveTimeout = 3000
  $udp.Connect('127.0.0.1', 7778); $q = [Text.Encoding]::ASCII.GetBytes('\status\')
  [void]$udp.Send($q, $q.Length); $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
  [void]$udp.Receive([ref]$ep); $udp.Close(); $utUp = $true
} catch {}
if ($utUp) { Write-Host "  ut99 already answering on 7778" }
else {
  Start-Process -FilePath 'C:\gameservers\ut99\System\UCC.exe' -WorkingDirectory 'C:\gameservers\ut99\System' `
    -ArgumentList 'server','DM-Deck16][?game=Botpack.DeathMatchPlus','port=7777','-log=server.log' -WindowStyle Minimized
  Write-Host "  started ut99 on UDP 7777"
}

Write-Host "== [3/4] Verify (loopback) =="
Start-Sleep -Seconds 12
foreach ($s in $cs) { Write-Host ("  {0}:{1} -> {2}" -f $s.Name, $s.Port, ($(if (Test-A2S $s.Port) {'RESPONDING'} else {'NO RESPONSE'}))) }
try {
  $udp = New-Object Net.Sockets.UdpClient; $udp.Client.ReceiveTimeout = 4000
  $udp.Connect('127.0.0.1', 7778); $q = [Text.Encoding]::ASCII.GetBytes('\status\')
  [void]$udp.Send($q, $q.Length); $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
  $r = $udp.Receive([ref]$ep)
  Write-Host ("  ut99:7778 -> RESPONDING (" + [Text.Encoding]::ASCII.GetString($r).Split('\')[4] + ")")
} catch { Write-Host "  ut99:7778 -> NO RESPONSE" }

Write-Host "== [4/4] retro_agent =="
$agentExe = Get-ChildItem 'C:\development\retro-agent' -Recurse -Filter 'retro_agent.exe' -EA SilentlyContinue | Select-Object -First 1
if ($agentExe -and -not (Get-Process retro_agent -EA SilentlyContinue)) {
  Start-Process -FilePath $agentExe.FullName -WorkingDirectory $agentExe.DirectoryName -WindowStyle Minimized
  Write-Host "  started retro_agent"
} else { Write-Host "  retro_agent ok" }
Write-Host ""
Write-Host "From a retro PC: CS LAN tab shows both (27017/27018 in the 27015-27020 scan);"
Write-Host "UT99 shows under Multiplayer -> LAN (beacon 8777) + Favorites 'NSC LAN - Retro Fleet UT99'."
