# PXE server inbound rules on the fleet LAN (whitebeast 192.168.1.249).
# proxyDHCP does not assign addresses; it only answers PXEClient requests.
$rules = @(
  @{ Name='Retro PXE - proxyDHCP 67';  Port=67 },
  @{ Name='Retro PXE - TFTP 69';       Port=69 },
  @{ Name='Retro PXE - PXE 4011';      Port=4011 }
)
foreach ($r in $rules) {
  Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
  New-NetFirewallRule -DisplayName $r.Name -Direction Inbound -Action Allow `
    -Protocol UDP -LocalPort $r.Port -Profile Any -Enabled True | Out-Null
  "added: $($r.Name) UDP $($r.Port)"
}
Get-NetFirewallRule -DisplayName 'Retro PXE*' | Select-Object DisplayName,Enabled,Direction,Action |
  Out-File -FilePath C:\development\pxe\firewall-status.txt -Encoding ascii
