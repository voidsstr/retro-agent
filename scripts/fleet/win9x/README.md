# Win9x helper tools — no C runtime

`fleet9x.exe` is a small Win32 GUI program for Windows 95/98 boxes:

    fleet9x attrib <path> <hex FILE_ATTRIBUTE_* mask>   # e.g. MSDOS.SYS 80, then back to 27
    fleet9x reboot                                      # forced reboot: kill console vetoes, ExitWindowsEx, keep pumping

Each run appends one line to `C:\RETRO_AGENT\FLEET9X.TXT`, so the result can be read back with `DOWNLOAD`.

Build (it must be `-nostdlib`, see below):

    i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -e _start@0 \
        -o fleet9x.exe fleet9x.c -lkernel32 -luser32 -s

Run it through the agent: `EXECW 30 C:\RETRO_AGENT\FLEET9X.EXE attrib ...`, or `LAUNCH` for `reboot`.

## The other tools (all found necessary on .243, 2026-09-24)

| tool | what it does | output |
|---|---|---|
| `pci9x [out] [noio]` | read-only PCI bus-0 config scan (mechanism #1: it only ever writes 0xCF8, never 0xCFC); reports each function's command register and BARs, with a verdict on any 3dfx BAR0 | `C:\RETRO_AGENT\PCI9X.TXT` |
| `reenum9x` | `CM_Reenumerate_DevNode` on the PCI bus (what agent 1.83.0's `PCIRESCAN` does) | `C:\RETRO_AGENT\REENUM.TXT` |
| `regdump9x <HKDD\|HKLM\|HKCC> <key> <out>` | recursive registry dump. Reads `HKEY_DYN_DATA`, the live devnode tree, which the agent's `REGREAD` cannot | the file you name |
| `wintext9x` | dumps every visible `#32770` dialog's controls: class, id, text, enabled, checked, rect. Lets you drive a wizard when 8-bpp screenshots are unreadable | `C:\RETRO_AGENT\WINTEXT.TXT` |
| `agentswap9x` | installs `retro_agent_new.exe` over a RUNNING agent (Win9x cannot replace a running exe). LAUNCH it, then send `QUIT`; it swaps, starts the new build, and rolls back if that build is not still running after 25 s | `C:\RETRO_AGENT\AGENTSWAP.TXT` |

All of them build with the same command (add `-ladvapi32` for regdump9x). GUI-subsystem exes are not waited for by `EXEC` on 9x, so poll `PROCLIST` until the process is gone, then `DOWNLOAD` the output.

## Why fleet9x exists (found on .243, Win98 SE, 2026-09-24)

* **Agents older than 1.82.1 cannot reboot a Win9x box.** `REBOOT` answered `OK` and did nothing: `CreateThread` with a NULL thread id fails on 95/98. Fixed in 1.82.1. Until a box runs that build, `fleet9x reboot` is the remote route.
* **The agent cannot write a read-only file**, and running DOS `ATTRIB` through `EXEC` starts a DOS VM, which is the pattern that has killed this single-threaded agent before. `fleet9x attrib` is plain Win32.
* **Build every Win9x helper without a C runtime.** On .243 the first, msvcrt-linked build of this tool sat in `PROCLIST`, wrote nothing, and left a `#32770` dialog named after itself. That orphaned dialog then **blocked a later `ExitWindowsEx`** until someone pressed Enter on it. The CRT-free rebuild never did this. The cause was never established: "an msvcrt export Win98 lacks" was checked and is wrong for two similar probes. So build `-nostdlib`, keep buffers larger than 4 KB static (otherwise `__chkstk_ms` gets linked in), and check `WINLIST` for `#32770` after running anything new.

## Caution

`reboot` is a forced reboot. On .243 on 2026-09-24 it went through (the agent's connection was reset as the session ended), but the machine did not come back without a person, exactly as a forced reboot did on 2026-08-31. The cause is not known: it may be the Win98 shutdown, or the new Voodoo 2, which hangs the PCI bus whenever software probes it. Arm the PXE hold first (`scripts/pxe/pxe_server.py --arm <mac>`) and do not use it on a box nobody can reach.
