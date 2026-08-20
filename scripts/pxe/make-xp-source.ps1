<#
  Build the Windows XP PXE payload:
    * expands the three RIS boot files out of the XP CD into the TFTP root
    * copies the whole CD to an SMB share the target can reach anonymously
    * writes winnt.sif pointing text-mode setup at that share

  The SMB source must be reachable with a NULL SESSION - Windows 11 no longer
  serves SMB1, so it cannot host this itself; the fleet NAS (MEDIASERVER,
  192.168.1.122) can, and does.

  Run elevated on the PXE host.
#>
param(
  [string]$Iso       = 'Z:\Files\OS\en_windows_xp_professional_with_service_pack_3_x86_cd_x14-80428.iso',
  [string]$TftpRoot  = 'C:\development\pxe\tftp',
  [string]$ShareUnc  = '\\192.168.1.122\files\Files\OS\XPSP3-PXE',
  [string]$SourceDev = '\Device\LanmanRedirector\192.168.1.122\files\Files\OS\XPSP3-PXE',
  [string]$OriSrc    = '\\192.168.1.122\files\Files\OS\XPSP3-PXE\I386'
)

New-Item -ItemType Directory -Force -Path $TftpRoot | Out-Null
$img = Mount-DiskImage -ImagePath $Iso -PassThru
Start-Sleep -Seconds 3
$drive = ($img | Get-Volume).DriveLetter
"mounted $Iso at ${drive}:"

# STARTROM.N12 is the no-F12 network bootstrap; SETUPLDR.EXE must be served as
# NTLDR (that is the name STARTROM asks the TFTP server for).
& expand "${drive}:\I386\STARTROM.N1_" "$TftpRoot\startrom.n12"
& expand "${drive}:\I386\SETUPLDR.EX_" "$TftpRoot\ntldr"
Copy-Item "${drive}:\I386\NTDETECT.COM" "$TftpRoot\ntdetect.com" -Force

$sif = @"
[Data]
    floppyless = "1"
    msdosinitiated = "1"
    OriSrc = "$OriSrc"
    OriTyp = "4"
    LocalSourceOnCD = 1

[SetupData]
    OsLoadOptions = "/fastdetect"
    SetupSourceDevice = "$SourceDev"
"@
$sif -replace "`n", "`r`n" | Set-Content -Path "$TftpRoot\winnt.sif" -Encoding Ascii -NoNewline

robocopy "${drive}:\" $ShareUnc /E /R:2 /W:2 /NFL /NDL /NP
Dismount-DiskImage -ImagePath $Iso | Out-Null
"payload ready: TFTP root $TftpRoot, install source $ShareUnc"
