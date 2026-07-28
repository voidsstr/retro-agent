# fxdbg - M4d on-card bring-up ladder driver (user-mode Win32 tool) + host ABI gate.
#
#   make            cross-compile fxdbg.exe (mingw) and run the host ABI selftest
#   make exe        just the Win32 tool
#   make test       just the host ABI selftest (no hardware, no Win32)
#   make clean
#
# The tool drives the FXDBG_* DrvEscapes (gbkdebug.h) over ExtEscape on the
# display DC; it is compile-verified here and run on .124 at M4d. It touches no
# card state itself - see fxdbg.c.

MINGW  ?= i686-w64-mingw32-gcc
CC     ?= cc

# Win32 build: C89-ish, warnings-as-errors, link GDI (ExtEscape/CreateDC).
WFLAGS  = -O2 -Wall -Wextra -Wno-long-long -std=gnu99
WLIBS   = -lgdi32

# Host selftest: -m32 so `unsigned long` is 4 bytes == the i386 driver ABI.
HFLAGS  = -m32 -O2 -Wall -Wextra -Wno-long-long -std=gnu99

.PHONY: all exe test clean
all: exe test

exe: fxdbg.exe
fxdbg.exe: fxdbg.c ../gbkdebug.h
	$(MINGW) $(WFLAGS) -o $@ fxdbg.c $(WLIBS)
	@echo "fxdbg: built $@ (Win32, mingw)"

test: selftest
	@./selftest

selftest: selftest.c ../gbkdebug.h
	$(CC) $(HFLAGS) -o $@ selftest.c

clean:
	rm -f fxdbg.exe selftest
