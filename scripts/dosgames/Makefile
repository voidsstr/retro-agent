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

# Rebuild data/GAMES.CAT from the share.
#
# The shipped artifact was generated before dos83() and survey_share.py's
# coms_shallow fix, so it still carries 14 rows whose launcher was truncated to
# 12 characters ("DECATHLON.EX") and not one of the 2,982 rows has a .COM
# launcher — impossible for a pre-1990 DOS library, and the reason those games
# are missing from the Available tab. Neither fix reaches the fleet until this
# runs, so it needs a target rather than living only in someone's shell history.
#
# NEEDS THE SHARE MOUNTED on this host (gvfs or cifs); override with
# DOSGAMES_SHARE. `make check-catalog` says whether the shipped copy is stale
# without needing the share at all.
SURVEY ?= /tmp/dos_survey.json

catalog:
	python3 survey_share.py $(SURVEY)
	python3 gen_catalog.py $(SURVEY) data/GAMES.CAT
	python3 tests/check_catalog.py data/GAMES.CAT

check-catalog:
	python3 tests/check_catalog.py data/GAMES.CAT

clean:
	rm -f dosgame.exe *.o *.err *.map
