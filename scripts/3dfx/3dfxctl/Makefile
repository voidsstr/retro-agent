# 3dfxctl - retro3dfx driver Control Panel (Win32 GUI, XP).
# Persistent registry-backed settings for the vintage 3dfx Voodoo driver stack:
# vsync, refresh, clock, gamma, SLI/AA, FIFO, and every other driver knob.
# Build with the same mingw toolchain as the rest of the fleet C tools.
#
#   make            -> 3dfxctl.exe (32-bit, static-libgcc, no extra DLLs)
#   make clean

CROSS   ?= i686-w64-mingw32-
CC       = $(CROSS)gcc
WINDRES  = $(CROSS)windres

# -mwindows: GUI subsystem (no console window).  -static-libgcc folds in the
# gcc runtime so the .exe needs no MinGW DLLs on the XP box.  Target Win2000+.
CFLAGS   = -Os -Wall -DWINVER=0x0500 -D_WIN32_WINNT=0x0500
LDFLAGS  = -mwindows -static-libgcc -lgdi32 -lcomctl32 -ladvapi32 -lcomdlg32 -luser32

all: 3dfxctl.exe

3dfxctl.exe: 3dfxctl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "built $@ - publish to the share + deploy per DEPLOY.md"

clean:
	rm -f 3dfxctl.exe 3dfxctl.o
