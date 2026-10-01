# R7 - Win98 SE IPX/SPX install: the MEASURED delta (2026-10-01, W98BUILD VM, Network applet)

Method: VM disk backed up stopped (w98.img.pre-ipx-2026-10-01). SourcePath pointed at C:\WIN98 (was
C:\WINDOWS\OPTIONS\CABS\ - restore if wanted). Control Panel > Network > Add > Protocol > Microsoft >
IPX/SPX-compatible Protocol. Windows bound it to BOTH adapters and offered Client for MS Networks over it.
Removed "IPX/SPX -> Dial-Up Adapter", unchecked Client for Microsoft Networks on IPX's Bindings tab
("You have not selected any drivers to bind with" -> No), Advanced > Frame Type = Ethernet 802.2, OK,
files copied from C:\WIN98, restart. Diff = live REGREAD walk + offline SYSTEM.DAT read of both images.

## Result
- After the restart: DRIVERS STATUS -> NETWORK\NWLINK\0001 "IPX/SPX-compatible Protocol" state ok problem 0.
  (Before the restart, the same devnode read problem 2 - normal: the static VxD loads at boot.)
- IPX inside a Win98 DOS box: IPXCHK.COM (INT 2Fh AX=7A00h, AL=FFh) -> "IPX PRESENT".
  ~/.retro-fleet/ipx-capture/tools/IPXCHK.COM (64 bytes; errorlevel 0 present / 1 absent).
- Files: nwlink.vxd 51,010 and wsipx.vxd 14,526 (1999-04-24) copied to C:\WINDOWS\SYSTEM.
  mswsosp.dll / rpcltc6 / rpclts6 / wsock.vxd were already present.

## Registry delta (types exact, from the offline read)
[HKLM\Enum\Network\NWLINK\0001]        (0001 only because the applet's first instance was the Dial-Up binding I removed)
  Class="NetTrans" Driver="NetTrans\0003" MasterCopy="Enum\Network\NWLINK\0001"
  DeviceDesc="IPX/SPX-compatible Protocol" CompatibleIDs="NWLINK" Mfg="Microsoft"
  ClassGUID="{4d36e975-e325-11ce-bfc1-08002be10318}" ConfigFlags=hex:10,00,00,00 Capabilities=hex:14,00,00,00
[HKLM\Enum\Network\NWLINK\0001\Bindings]   EMPTY (no client over IPX - this is what we want)
[HKLM\Enum\PCI\<the NIC>\Bindings]  "NWLINK\0001"=""          <- adapter -> protocol, the activating link
[HKLM\System\CurrentControlSet\Services\Class\NetTrans\0003]   (first free NetTrans index)
  DriverDesc="IPX/SPX-compatible Protocol" InfSection="NWLINK.ndi" InfPath="NETTRANS.INF"
  ProviderName="Microsoft" DriverDate=" 4-23-1999" DevLoader="*ndis" DeviceVxDs="nwlink.vxd"
  Network_Id="0" Frame_Type="1"            <- REG_SZ strings. 1 = Ethernet 802.2
  \Ndi  DeviceID="NWLINK" HelpText=... InstallInf="" MaxInstance=8(REG_SZ "8"?) NdiInstaller="netdi.dll,NwlinkNdiProc" StaticVxD="nwlink.vxd"
  \Ndi\Interfaces DefLower=LowerRange=Lower="ndis2,ndis3,odi"  DefUpper=UpperRange=Upper="ipx,ipxDHost,winsock"
  \Ndi\Install @="NWLINK.Install"   \Ndi\Remove @="NWLINK.Remove"
  \Ndi\params\{cachesize,forceeven,maxconnect,maxsockets} location="System\CurrentControlSet\Services\Vxd\NWLink"
  \Ndi\params\Frame_Type  @="1" default="4" ParamDesc="Frame Type" type="enum"
      \enum 0=Ethernet 802.3 1=Ethernet 802.2 2=Ethernet II 4=Auto 5=Token Ring 6=Token Ring SNAP
  \Ndi\params\Network_Id  @="0" base=hex:16 default="0" flag=hex:20,00,00,00 ParamDesc="Network Address" type="dword"
  \NDIS LogDriverName="NWLINK" MajorNdisVersion=hex:03 MinorNdisVersion=hex:0a
  \NDIS\NDIS2 DriverName="nwlink$" FileName="*nwlink"
[HKLM\System\CurrentControlSet\Services\VxD\NWLINK]
  StaticVxD="nwlink.vxd" Start=hex:00 NetClean=hex:01 cachesize="0"
  \Ndi\params\... (UI only: cachesize/forceeven/maxconnect/maxsockets descriptors)
[HKLM\System\CurrentControlSet\Services\VxD\Winsock]  "IPX/SPX Winsock Provider"="wsipx.vxd"
[HKLM\System\CurrentControlSet\Services\WinSock2\Providers\IPX]  ProviderName="Microsoft IPX"
Winsock 2 catalog (Protocol_Catalog9): done IMMEDIATELY at OK time (pre-reboot capture already had 9 entries,
RunOnce empty, no QueuedAPI). MSWSOSP {FF017DE1-CAE9-11CF-8A99-00AA0062C609} was DEINSTALLED (its one hidden
"MS.w95.spi.osp" entry, catalog id 1001, was entry 1) and REINSTALLED with 4 protocols, appended after the
existing msafd/rsvp entries (catalog ids 1012-1015, Next_Catalog_Entry_ID 1012 -> 1016):
  MS.w95.spi.spx      af=6 SOCK_STREAM(1)    proto=1256 flags1=0x2001e pflags=0x8
  MS.w95.spi.spx/seq  af=6 SOCK_SEQPACKET(5) proto=1256 flags1=0x2003e pflags=0x8
  MS.w95.spi.ipx      af=6 SOCK_DGRAM(2)     proto=1000 maxoff=255 flags1=0x20609 pflags=0x8
  MS.w95.spi.osp      af/type/proto=0xFACEFACE (hidden) flags1=0x20609 pflags=0xc
  path C:\WINDOWS\SYSTEM\mswsosp.dll. Full 888-byte PackedCatalogItem blobs: offline-post-ipx.txt.
Unchanged: Software\...\Setup\NetSetup (WSock2Installed=1), RunOnce (empty), Network\Logon.

## Gotchas seen
- The applet binds IPX to the Dial-Up Adapter too and offers Client for MS Networks over IPX; both removed.
- After a hard kill of the agent console (End Task on a hung REGEDIT /e tty), the next boot stopped at
  "Enter Network Password" (user retro, blank) once; the following agent REBOOT logged on by itself.
- REGEDIT /e under EXECW wrote 0 bytes and left a hung tty window - do not use it; read SYSTEM.DAT offline.
