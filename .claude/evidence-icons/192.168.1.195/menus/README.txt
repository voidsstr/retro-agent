2026-09-28 read-only probe of the live desktop listview on .195 (medium icons, after the hand fix), via EXEC powershell -EncodedCommand lvprobe.ps1:
progman=655520 workerw=0 defview=393350 lv=393370 style=0x56003B40 type=0 getview=0 count=119

Registry reads (REGREAD HKCU):
.195 Shell\BagMRU NodeSlot=4; Shell\Bags\4\Desktop FFlags=0x40200225 Mode=1 LogicalViewMode=3 IconSize=48; Shell\Bags\1\Desktop FFlags=0x221 only (agent-created)
.124/.110 (XP SP3) Shell\BagMRU NodeSlot=1; ShellNoRoam\BagMRU has no NodeSlot; Shell\Bags\1\Desktop Mode=1 FFlags=549 (0x225)
.243 (Win98) Shell\BagMRU absent; Shell\Bags\1\Desktop FFlags=545 only
