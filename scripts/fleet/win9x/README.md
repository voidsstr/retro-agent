# fleet9x — two Win9x chores, with no C runtime

`fleet9x.exe` is a small Win32 GUI program for Windows 95/98 boxes:

    fleet9x attrib <path> <hex FILE_ATTRIBUTE_* mask>   # e.g. MSDOS.SYS 80, then back to 27
    fleet9x reboot                                      # forced reboot: kill console vetoes, ExitWindowsEx, keep pumping

Each run appends one line to `C:\RETRO_AGENT\FLEET9X.TXT`, so the result can be read back with `DOWNLOAD`.

Build (it must be `-nostdlib`, see below):

    i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -e _start@0 \
        -o fleet9x.exe fleet9x.c -lkernel32 -luser32 -s

Run it through the agent: `EXECW 30 C:\RETRO_AGENT\FLEET9X.EXE attrib ...`, or `LAUNCH` for `reboot`.

## Why it exists (found on .243, Win98 SE, 2026-09-24)

* **Agents older than 1.82.1 cannot reboot a Win9x box.** `REBOOT` answered `OK` and did nothing: `CreateThread` with a NULL thread id fails on 95/98. Fixed in 1.82.1. Until a box runs that build, `fleet9x reboot` is the remote route.
* **The agent cannot write a read-only file**, and running DOS `ATTRIB` through `EXEC` starts a DOS VM, which is the pattern that has killed this single-threaded agent before. `fleet9x attrib` is plain Win32.
* **A mingw-w64 program linked against msvcrt does not start on Win98 SE.** Its startup code imports a CRT function that Win98's `MSVCRT.DLL` lacks. The loader then shows a modal "missing export" dialog that nobody sees: the process sits in `PROCLIST`, writes nothing, and `EXECW` returns empty. Worse, the orphaned dialog **blocks a later `ExitWindowsEx`** until someone presses Enter on it. So this tool uses no CRT (`-nostdlib`, `wsprintfA`, `CreateFileA`), and it imports only KERNEL32 and USER32. **Build any throwaway Win9x probe the same way.**

## Caution

`reboot` is a forced reboot. On .243 on 2026-09-24 it went through (the agent's connection was reset as the session ended), but the machine did not come back without a person, exactly as a forced reboot did on 2026-08-31. The cause is not known: it may be the Win98 shutdown, or the new Voodoo 2, which hangs the PCI bus whenever software probes it. Arm the PXE hold first (`scripts/pxe/pxe_server.py --arm <mac>`) and do not use it on a box nobody can reach.
