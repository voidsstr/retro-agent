# Makefile for retrobot.so -- the GoldSrc (CS 1.6 / The Specialists) Metamod
# adapter for the neural game-bot policy server (see ../../README.md and
# ../../../docs/game-ai-bots-plan.md).
#
# GoldSrc is a 32-bit engine. This target links a 32-bit .so unconditionally
# (-m32) -- there is no 64-bit HLDS to build for.
#
# *** This Makefile has never successfully completed a build on the dev host
# *** it was written on: that host has gcc/g++ but NOT the 32-bit multilib
# *** package (libc6-dev-i386 / gcc-multilib), and installing it needs
# *** interactive sudo the build session did not have. See README.md
# *** "Build status" before assuming this Makefile works -- `make
# *** check-toolchain` tells you in one command whether your host can even
# *** attempt it, and names the exact apt command if not.
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt-get install gcc-multilib g++-multilib libc6-dev-i386
#
# Usage:
#   make deps              # clone HLSDK + metamod-hl1 into build/ (gitignored)
#   make check-toolchain    # confirm the 32-bit compiler actually works
#   make                    # build build/retrobot.so
#   make test-core           # build & run the engine-independent unit tests
#                             (this part DOES work today -- see ../../../tests)

SHELL := /bin/bash

HERE       := $(shell cd "$(dir $(lastword $(MAKEFILE_LIST)))" && pwd)
BUILD      := $(HERE)/build
HLSDK      := $(BUILD)/hlsdk
METAMOD    := $(BUILD)/metamod-hl1/metamod
OUT        := $(BUILD)/retrobot.so

CXX        ?= g++
CC         ?= gcc

INCLUDES := -I$(HLSDK)/common -I$(HLSDK)/dlls -I$(HLSDK)/engine \
            -I$(HLSDK)/pm_shared -I$(HLSDK)/public -I$(METAMOD) \
            -I$(HERE) -I$(HERE)/..

# -m32: GoldSrc is a 32-bit engine, full stop.
# -fPIC -shared: building a loadable .so.
# -Dlinux -D_LINUX -DPOSIX: several HLSDK/metamod-hl1 files branch on these
#   (rather than the compiler-predefined `linux`, which strict -std modes
#   don't define); harmless to define unconditionally on Linux.
# No -Werror: this is 2001-vintage third-party SDK code (deprecated
#   string-literal-to-char* conversions throughout); matching how every real
#   metamod plugin build treats it, including metamod-hl1's own Makefile.
# -fpermissive: REQUIRED, not cosmetic. metamod-hl1's own sdk_util.h (not
#   ours to edit) passes a `const char *` function parameter into APIs
#   declared `char *` -- valid, sloppy C++98, a hard error under modern
#   G++'s default strictness. Confirmed by an actual build: without this
#   flag, sdk_util.h itself fails to compile with "invalid conversion from
#   'const char*' to 'char*'". This is the standard, widely-documented
#   requirement for building any 2001-era GoldSrc/Metamod SDK code with a
#   current compiler, not something specific to this plugin.
CXXFLAGS := -m32 -fPIC -shared -Wall -O2 -fpermissive $(INCLUDES) \
            -Dlinux -D_LINUX -DPOSIX -fno-exceptions
CFLAGS   := -m32 -fPIC -Wall -O2 $(INCLUDES) -Dlinux -D_LINUX -DPOSIX

SRCS_CXX := retrobot_engine.cpp
SRCS_C   := retrobot_core.c ../gb_client.c

OBJS := $(BUILD)/retrobot_engine.o $(BUILD)/sdk_util.o $(BUILD)/retrobot_core.o $(BUILD)/gb_client.o

.PHONY: all deps check-toolchain clean test-core

all: check-toolchain deps $(OUT)

$(BUILD):
	mkdir -p $(BUILD)

# --- fetch third-party sources (NOT committed -- see ../../../.gitignore) --
deps: $(BUILD)
	@if [ ! -d "$(HLSDK)/.git" ]; then \
		echo "cloning HLSDK into $(HLSDK) ..."; \
		git clone --depth 1 https://github.com/ValveSoftware/halflife.git "$(HLSDK)"; \
	else echo "HLSDK already present at $(HLSDK)"; fi
	@if [ ! -d "$(BUILD)/metamod-hl1/.git" ]; then \
		echo "cloning metamod-hl1 into $(BUILD)/metamod-hl1 ..."; \
		git clone --depth 1 https://github.com/alliedmodders/metamod-hl1.git "$(BUILD)/metamod-hl1"; \
	else echo "metamod-hl1 already present at $(BUILD)/metamod-hl1"; fi

# --- sanity-check the 32-bit toolchain before wasting a build attempt on it -
check-toolchain:
	@echo "== checking for a working 32-bit C/C++ toolchain =="
	@printf 'int main(void){return 0;}\n' > $(BUILD)/.32check.c 2>/dev/null || \
		(mkdir -p $(BUILD) && printf 'int main(void){return 0;}\n' > $(BUILD)/.32check.c)
	@if $(CC) -m32 $(BUILD)/.32check.c -o $(BUILD)/.32check 2>$(BUILD)/.32check.log; then \
		echo "  OK: $(CC) -m32 can compile and link a trivial program"; \
		rm -f $(BUILD)/.32check $(BUILD)/.32check.c $(BUILD)/.32check.log; \
	else \
		echo "  MISSING: $(CC) -m32 cannot build a 32-bit binary on this host."; \
		echo "  This is the exact wall this adapter hit during development --"; \
		echo "  see README.md 'Build status'. Typical cause: no 32-bit libc"; \
		echo "  dev headers/libs (only the runtime, libc6-i386, may be present)."; \
		echo "  Fix (Debian/Ubuntu, needs sudo):"; \
		echo "      sudo apt-get install gcc-multilib g++-multilib libc6-dev-i386"; \
		echo "  Compiler output:"; \
		sed 's/^/    /' $(BUILD)/.32check.log; \
		rm -f $(BUILD)/.32check.c $(BUILD)/.32check.log; \
		exit 1; \
	fi

$(BUILD)/retrobot_engine.o: retrobot_engine.cpp retrobot_core.h ../../gamebots_schema.h ../gb_client.h | $(BUILD) deps
	$(CXX) $(CXXFLAGS) -c $< -o $@

# metamod-hl1 does not ship a linkable metamod runtime for plugins to share --
# every plugin compiles its OWN copy of these SDK utility routines
# (UTIL_LogPrintf, ENTITY_KEYVALUE, ...) straight from the metamod-hl1
# checkout. Confirmed necessary by an actual link/dlopen attempt: without
# this, retrobot.so builds and links (nothing catches it at build time --
# they're only USED, never redefined, in this TU) but fails to dlopen with
# "undefined symbol: UTIL_LogPrintf". metamod-hl1's own stub_plugin lists
# sdk_util.cpp as one of ITS sources for exactly this reason.
$(BUILD)/sdk_util.o: $(METAMOD)/sdk_util.cpp | $(BUILD) deps
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/retrobot_core.o: retrobot_core.c retrobot_core.h ../../gamebots_schema.h | $(BUILD)
	$(CC) -m32 -fPIC -Wall -O2 -std=c11 -I$(HERE) -I$(HERE)/.. -c $< -o $@

$(BUILD)/gb_client.o: ../gb_client.c ../gb_client.h ../../gamebots_schema.h | $(BUILD)
	$(CC) -m32 -fPIC -Wall -O2 -I$(HERE)/.. -c $< -o $@

$(OUT): $(OBJS)
	$(CXX) -m32 -shared -fPIC -o $@ $(OBJS) -lm
	@echo "built $(OUT)"
	@echo "NOT deployed anywhere -- see README.md for how to test this against"
	@echo "a throwaway HLDS instance before touching the live servers."

# --- the part of this adapter that IS tested today: the engine-independent
# core, via tests/native/test_gamebots_goldsrc.c (true-source, no HLSDK, no
# -m32 needed). Run from the repo root: bash tests/run_all.sh
test-core:
	@echo "See ../../../tests/native/test_gamebots_goldsrc.c and"
	@echo "'bash ../../../tests/run_all.sh' -- that is the tested half of"
	@echo "this adapter. This target exists just to point at it."

clean:
	rm -rf $(BUILD)/*.o $(OUT) $(BUILD)/.32check*
