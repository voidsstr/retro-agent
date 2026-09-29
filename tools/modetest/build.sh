#!/bin/bash
# Build modetest.exe so a GENUINE PENTIUM on Windows 98 can load and run it.
#
# The flags are not optional, and each one is a lesson this repo already paid for:
#   -march=pentium -mno-sse   .243 is a P54C: no CMOV (Pentium Pro+), no MMX,
#                             no SSE. provisioning/fleetres/build.sh found 78
#                             CMOVs in a default i686 build of FLEETRES.EXE.
#   -nostdlib, own entry      no msvcrt: a normal mingw build left an orphaned
#                             #32770 dialog on .243 that later blocked
#                             ExitWindowsEx (memory: win98-mingw-msvcrt-hangs).
#   -lkernel32 -luser32 -lgdi32 ONLY
#                             a static import Win9x lacks kills the exe at load,
#                             before main, with no log (CLAUDE.md). advapi32,
#                             ddraw and d3d9 are LoadLibrary'd at run time.
#   -lgcc                     vcr_pace.h's 64-bit millisecond clock divides a
#                             ULONGLONG (__udivdi3); libgcc's has no CMOV
#                             (checked with objdump 2026-09-29), and the check
#                             below would catch it if a toolchain changed that.
#   -fno-tree-loop-distribute-patterns
#                             keeps gcc from turning our own memset/memcpy
#                             loops back into calls to memset/memcpy.
#
# SELF-CHECKS (a build that fails them exits non-zero): the PE import table
# names only KERNEL32/USER32/GDI32, and there are at most 2 CMOVs (libgcc's
# two dead pseudo-reloc helpers, as agent/Makefile documents).
set -eu

cd "$(dirname "$0")"
CC="${CC:-i686-w64-mingw32-gcc}"
OBJDUMP="${OBJDUMP:-i686-w64-mingw32-objdump}"
OUT="${OUT:-modetest.exe}"

$CC -Os -s -Wall -Wextra -Wno-unused-function -Wno-cast-function-type \
    -march=pentium -mtune=pentium -mno-sse -mno-mmx \
    -fno-stack-protector -fno-builtin -fno-tree-loop-distribute-patterns \
    -fno-asynchronous-unwind-tables \
    -DWINVER=0x0500 -D_WIN32_WINNT=0x0500 \
    -nostdlib -Wl,-e,_start@0 -Wl,--subsystem,windows:4.0 \
    -o "$OUT" modetest.c -lkernel32 -luser32 -lgdi32 -lgcc

fail=0
dlls=$($OBJDUMP -p "$OUT" | awk '/DLL Name:/ {print toupper($3)}' | sort -u | tr '\n' ' ')
for d in $dlls; do
    case "$d" in
        KERNEL32.DLL|USER32.DLL|GDI32.DLL) ;;
        *) echo "BUILD FAILED: $OUT imports $d - only KERNEL32/USER32/GDI32 may be static" >&2
           fail=1 ;;
    esac
done
# Every IMPORTED FUNCTION must be on this list, each one a Win95/98-era export
# of its DLL. A DLL-name check alone would pass an NT-only KERNEL32 entry point
# (TryEnterCriticalSection, SignalObjectAndWait, ...) - and on Win98 one
# unresolved import kills the exe at load, before any log line exists. A new
# import fails the build until someone checks it against Win98 SE and adds it.
allowed="CloseHandle CreateDirectoryA CreateFileA CreateMutexA CreateThread
EnterCriticalSection ExitProcess GetCommandLineA GetCurrentThread
GetCurrentThreadId GetFileAttributesA GetLastError GetModuleFileNameA
GetModuleHandleA GetProcAddress GetStdHandle GetSystemTimeAsFileTime GetTickCount
GetVersionExA InitializeCriticalSection LeaveCriticalSection LoadLibraryA
QueryPerformanceCounter QueryPerformanceFrequency ReadFile ReleaseMutex SetErrorMode
SetFilePointer SetThreadPriority SetUnhandledExceptionFilter Sleep
WaitForSingleObject WriteFile lstrcmpiA lstrcpyA lstrcpynA
AttachThreadInput BringWindowToTop ChangeDisplaySettingsA CreateWindowExA
DefWindowProcA DestroyWindow DispatchMessageA EnumDisplaySettingsA GetDC
GetForegroundWindow GetSystemMetrics GetWindowThreadProcessId MsgWaitForMultipleObjects
PeekMessageA
RegisterClassA ReleaseDC SetCursor SetFocus SetForegroundWindow ShowWindow
TranslateMessage UpdateWindow wsprintfA wvsprintfA
GetDeviceCaps GetStockObject"
funcs=$($OBJDUMP -p "$OUT" | awk '/DLL Name:/ {inimp=1; next}
    inimp && /^\t[0-9a-f]+ +<none> +[0-9a-f]+ +/ {print $4}' | sort -u)
for f in $funcs; do
    case " $(echo $allowed) " in
        *" $f "*) ;;
        *) echo "BUILD FAILED: $OUT imports $f - not on the Win95/98 allowlist in build.sh" >&2
           fail=1 ;;
    esac
done
[ -n "$funcs" ] || { echo "BUILD FAILED: could not read $OUT's imported functions" >&2; fail=1; }
cmov=$($OBJDUMP -d -M intel --no-show-raw-insn "$OUT" | grep -cE '\bcmov' || true)
if [ "$cmov" -gt 2 ]; then
    echo "BUILD FAILED: $OUT has $cmov CMOV instructions - a Pentium P54C faults on the first" >&2
    fail=1
fi
sub=$($OBJDUMP -p "$OUT" | awk '/^Subsystem/ {print $2}')
osv=$($OBJDUMP -p "$OUT" | awk '/^MajorOSystemVersion/ {maj=$2} /^MinorOSystemVersion/ {min=$2} END {print maj"."min}')
ssv=$($OBJDUMP -p "$OUT" | awk '/^MajorSubsystemVersion/ {maj=$2} /^MinorSubsystemVersion/ {min=$2} END {print maj"."min}')
[ "$fail" = 0 ] || exit 1
echo "$OUT: $(stat -c%s "$OUT") bytes; imports: $dlls; CMOV: $cmov (<=2 dead ok);" \
     "subsystem $sub, subsystem version $ssv, OS version $osv"
