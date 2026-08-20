# Register the PXE server as a Windows startup task (runs as SYSTEM, no logon needed).
# Run elevated:  powershell -ExecutionPolicy Bypass -File install-task.ps1
$py     = 'C:\Python314\pythonw.exe'
$script = 'C:\development\retro-agent\scripts\pxe\pxe_server.py'
$config = 'C:\development\pxe\pxe_config.json'

$action  = New-ScheduledTaskAction -Execute $py -Argument "$script --config $config" `
           -WorkingDirectory 'C:\development\pxe'
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
             -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)
Unregister-ScheduledTask -TaskName 'RetroPXE' -Confirm:$false -ErrorAction SilentlyContinue
Register-ScheduledTask -TaskName 'RetroPXE' -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Description 'Retro fleet PXE server (proxyDHCP + TFTP) for network OS installs' | Out-Null
Get-ScheduledTask -TaskName 'RetroPXE' | Select-Object TaskName,State
