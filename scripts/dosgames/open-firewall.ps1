<#
    open-firewall.ps1 - allow the fleet's DOS boxes to reach the DOS games
    HTTP bridge on this machine.

    Scoped deliberately: inbound TCP 8181 only, and only from the fleet
    subnet, so the port is not exposed on any other network this machine
    joins. Run elevated.

    Undo with:  Remove-NetFirewallRule -DisplayName 'Retro DOS Games HTTP (8181)'
#>
param(
    [int]$Port = 8181,
    [string]$Subnet = '192.168.1.0/24',
    [string]$Name = 'Retro DOS Games HTTP (8181)'
)

$log = Join-Path $env:TEMP 'fwrule.log'
try {
    Get-NetFirewallRule -DisplayName $Name -ErrorAction SilentlyContinue |
        Remove-NetFirewallRule -ErrorAction SilentlyContinue

    New-NetFirewallRule -DisplayName $Name `
        -Direction Inbound -Protocol TCP -LocalPort $Port `
        -RemoteAddress $Subnet -Profile Any -Action Allow `
        -Description 'Retro fleet: DOS game manager archive fetch (mTCP HTGET)' `
        -ErrorAction Stop | Out-Null

    "CREATED $Name tcp/$Port from $Subnet" | Out-File -FilePath $log -Encoding ascii
} catch {
    "FAILED: $($_.Exception.Message)" | Out-File -FilePath $log -Encoding ascii
}
