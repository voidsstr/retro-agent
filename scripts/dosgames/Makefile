# Makefile for DOSGAME.EXE — Open Watcom cross-compile from Linux.
# Needs the toolchain at ~/development/toolchain-dos/watcom (see README.md).
WATCOM ?= $(HOME)/development/toolchain-dos/watcom
export WATCOM
export PATH := $(WATCOM)/binl64:$(PATH)
export INCLUDE := $(WATCOM)/h

all: dosgame.exe

# -k8192: Watcom's default DOS stack is 2K. The scan chain
# (main -> scan_local -> scan_root -> scan_game_dir -> pick_launcher, each
# with its own MAX_PATH_L buffers and a find_t) plus Watcom's sprintf came
# close enough to that to be a hazard on a real box, where a blown stack is a
# hung machine. 8K costs nothing: the whole image is under 80K.
dosgame.exe: dosgame.c
	wcl -bcl=dos -ml -os -q -wx -k8192 -fe=dosgame.exe dosgame.c

# Build + run the DOS-side regression tests headlessly in DOSBox.
test: dosgame.exe
	bash tests/run_dos_tests.sh

clean:
	rm -f dosgame.exe *.o *.err *.map
