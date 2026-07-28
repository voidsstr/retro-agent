# fxD3D host display driver (M3).
#
#   make            - cross-compile the portable glue for Win32 + build & RUN
#                     the host glue test (no DDK, no hardware).
#   make winobj     - just the Win32 glue object
#   make test       - just the host glue test
#
# The FULL loadable driver (nt/ and win9x/) needs the platform DDK and is built
# with nt/SOURCES under the DDK `build` env (or on a provisioned XP box via the
# agent). See README.md. This Makefile builds/tests everything that is
# DDK-independent so the bridge is provably correct here.

CROSS   ?= i686-w64-mingw32-
WINCC    = $(CROSS)gcc
HOSTCC  ?= gcc

HAL      = ../d3dhal
SDK      = ../glide-sdk
INC      = -I. -I$(HAL)/include -I$(HAL) -I$(SDK)/include
WINCFLAGS  = -Os -Wall -Wextra -DWIN32 -D__WIN32__ $(INC)
HOSTCFLAGS = -Os -Wall -Wextra $(INC) -Itest

HALSRC = $(HAL)/d3dhal_state.c $(HAL)/d3dhal_tex.c $(HAL)/d3dhal_prim.c $(HAL)/d3dhal_ddi.c $(HAL)/d3dhal_dp2real.c

all: winobj test

winobj: ddi_glue.win.o
	@echo "fxD3D DDI glue cross-compiled for Win32: $<"

ddi_glue.win.o: ddi_glue.c ddi_glue.h $(HAL)/include/fxd3d.h
	$(WINCC) $(WINCFLAGS) -c -o $@ $<

test: test_glue
	@echo "== running host glue test =="
	./test_glue

test_glue: ddi_glue.c $(HALSRC) test/test_glue.c $(HAL)/test/stub_backend.c
	$(HOSTCC) $(HOSTCFLAGS) -o $@ ddi_glue.c $(HALSRC) test/test_glue.c \
	    $(HAL)/test/stub_backend.c -lm

clean:
	rm -f *.win.o test_glue test_glue.exe
