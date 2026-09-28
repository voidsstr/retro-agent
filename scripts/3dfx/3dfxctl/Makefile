# 3dfxctl - the 3dfx Control Panel (Win32 GUI, Windows XP SP3 / 2000).
# Stack-aware: OUR stack (vcr-kmd + h5 Glide + MesaFX ICD) or the vintage
# 3dfxvs driver. See DEPLOY.md for what it controls, the deploy and the verify.
#
#   make            -> 3dfxctl.exe (32-bit, static, no DLLs beyond XP's own)
#   make icon       -> redraw 3dfxctl.ico (python3 make_icon.py)
#   make clean
#
# -mwindows: GUI subsystem. -static: libgcc/libwinpthread folded in, so the
# exe needs nothing but kernel32/user32/gdi32/advapi32/comctl32/msvcrt, all of
# which XP SP3 has (tests/python/test_3dfxctl_build.py checks every import
# against the real XP SP3 export tables in xp_exports/). uxtheme.dll is
# LoadLibrary'd, never imported: a box running the classic theme still starts.
# -march=i586: nothing the fleet's oldest XP CPU lacks (.124 has no SSE2).

CROSS   ?= i686-w64-mingw32-
CC       = $(CROSS)gcc
WINDRES  = $(CROSS)windres

VCRKMD   = ../../../voodoo-cleanroom/vcr-kmd
INC      = -I. -I$(VCRKMD)/include -I$(VCRKMD)/tools

CFLAGS   = -Os -Wall -march=i586 -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -D_WIN32_IE=0x0600 \
           -Wno-unused-function $(INC)
LDFLAGS  = -mwindows -static -lcomctl32 -lgdi32 -luser32 -ladvapi32

# the gate and the driver contract are compiled in: rebuild when they change
DEPS     = 3dfxctl.c ctl_logic.h $(VCRKMD)/tools/vcr_pace.h $(VCRKMD)/include/vcr_ioctl.h \
           $(VCRKMD)/include/vcr_types.h $(VCRKMD)/include/vcr_log.h

all: 3dfxctl.exe

3dfxctl.res.o: 3dfxctl.rc 3dfxctl.ico 3dfxctl.manifest
	$(WINDRES) -O coff 3dfxctl.rc -o $@

3dfxctl.exe: $(DEPS) 3dfxctl.res.o
	$(CC) $(CFLAGS) -o $@ 3dfxctl.c 3dfxctl.res.o $(LDFLAGS)
	@echo "built $@ - deploy + verify per DEPLOY.md"

icon:
	python3 make_icon.py

clean:
	rm -f 3dfxctl.exe 3dfxctl.res.o

.PHONY: all icon clean
