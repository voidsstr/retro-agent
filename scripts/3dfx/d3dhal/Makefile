# fxD3D - clean-room Direct3D HAL for Voodoo 3/4/5, on the Glide backend.
# See docs/3dfx-d3d-hal-design.md.
#
#   make            - cross-compile the HAL translation objects for Win32
#                     (proves the driver-side code compiles) + build/run the
#                     host unit test (proves the DP2->Glide logic is correct).
#   make winobj     - just the Win32 .o's
#   make test       - just the host unit test
#   make clean
#
# The full loadable driver needs the DDK host (9x minivdd / NT miniport) and is
# built with -DHAVE_DDK inside that tree; here we build the portable core.

CROSS   ?= i686-w64-mingw32-
WINCC    = $(CROSS)gcc
HOSTCC  ?= gcc

SDK      = ../glide-sdk
INC      = -I$(SDK)/include -Iinclude -I.
WINCFLAGS  = -Os -Wall -Wextra -DWIN32 -D__WIN32__ $(INC)
HOSTCFLAGS = -Os -Wall -Wextra -Iinclude -I. -Itest -I$(SDK)/include

HALSRC = d3dhal_state.c d3dhal_tex.c d3dhal_prim.c d3dhal_ddi.c d3dhal_dp2real.c
WINOBJ = $(HALSRC:.c=.win.o)

all: winobj test

# --- Win32 driver-core objects (compile-only proof) ------------------------
winobj: $(WINOBJ)
	@echo "fxD3D core cross-compiled for Win32: $(WINOBJ)"

%.win.o: %.c include/fxd3d.h include/fxd3d_dp2.h glidebackend.h
	$(WINCC) $(WINCFLAGS) -c -o $@ $<

# --- host unit tests (run on the build machine) ----------------------------
test: test_dp2 test_dp2real
	@echo "== running host unit tests =="
	./test_dp2
	./test_dp2real

test_dp2: $(HALSRC) test/test_dp2.c test/stub_backend.c include/fxd3d.h glidebackend.h
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $(HALSRC) test/test_dp2.c test/stub_backend.c -lm

test_dp2real: $(HALSRC) test/test_dp2real.c test/stub_backend.c include/fxd3d.h include/fxd3d_dp2.h glidebackend.h
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $(HALSRC) test/test_dp2real.c test/stub_backend.c -lm

clean:
	rm -f *.win.o test_dp2 test_dp2.exe test_dp2real test_dp2real.exe
