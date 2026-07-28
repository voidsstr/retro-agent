# gbkernel-test - host-side (native gcc) build + run of the fxD3D kernel
# Glide backend's pure-logic core (../gbk/gbk_*.c), per
# docs/3dfx-gbkernel-design.md ("Pure-logic parts get host tests").
#
#   make            build everything and run all tests (PASS/FAIL per module)
#   make build      just compile
#   make clean
#
# The module objects are compiled with -nostdinc + kernel-ish strictness so
# a CRT include or C99-ism in ../gbk breaks HERE, on the host, before it can
# break the VC6/DDK build. Test drivers may use stdio (test_common.h).

CC      ?= gcc
BUILD   := obj
MODDIR  := ../gbk

MODULES := packet layout state fifo surf

# kernel-clean discipline for the modules: C89, no system headers at all.
MODCFLAGS  := -std=c89 -pedantic -Wall -Wextra -Werror -O2 -nostdinc
# tests are host programs: same warnings, CRT allowed.
TESTCFLAGS := -std=c89 -pedantic -Wall -Wextra -Werror -O2

# ILP32 shift-overflow gate. The real target ABIs (mingw i686-w64-mingw32,
# VC6 /Zp8) have 32-bit `long`, where a register-bit mask that reaches bit 31
# overflows signed `long` when shifted -- undefined behavior the LP64 host
# build CANNOT see (there `long` is 64-bit and the value is representable). So
# additionally compile the self-contained modules under the 32-bit ABI with
# -Wshift-overflow=2 -Werror; that is exactly what reproduced the SST_ALPHAREF
# finding. Best-effort: SKIPPED (not failed) when no -m32 multilib is present,
# so the default build stays green on hosts without it.
M32_OK := $(shell printf 'int main(void){return 0;}' | \
            $(CC) -m32 -xc - -o /dev/null 2>/dev/null && echo yes)

MOD_OBJS  := $(patsubst %,$(BUILD)/gbk_%.o,$(MODULES))
TEST_BINS := $(patsubst %,$(BUILD)/test_gbk_%,$(MODULES))

.PHONY: all build run clean ilp32gate
.SECONDARY: $(MOD_OBJS)   # keep module objects across runs

all: run

build: $(TEST_BINS)

# ILP32-ABI compile of the modules with shift-overflow promoted to an error.
# A bit-31 mask written signed (e.g. the old `#define SST_ALPHAREF (0xFFL<<24)`)
# fails HERE, which the LP64 host build cannot catch.
ilp32gate: | $(BUILD)
ifeq ($(M32_OK),yes)
	@echo "gbkernel-test: ILP32 shift-overflow gate (-m32 -Wshift-overflow=2)"
	@for m in $(MODULES); do \
	    $(CC) $(MODCFLAGS) -m32 -Wshift-overflow=2 \
	        -c -o $(BUILD)/gbk_$$m.m32.o $(MODDIR)/gbk_$$m.c || exit 1; \
	done
	@echo "gbkernel-test: ILP32 gate PASS"
else
	@echo "gbkernel-test: ILP32 gate SKIPPED (no -m32 multilib; host is LP64)"
endif

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/gbk_%.o: $(MODDIR)/gbk_%.c $(MODDIR)/gbk.h ../hw/h3hw.h | $(BUILD)
	$(CC) $(MODCFLAGS) -c -o $@ $<

# link ALL module objects into each test: the modules are one unit
# (e.g. gbk_fifo.c uses gbk_packet.c's gbk_pkt0_jmp_local)
$(BUILD)/test_gbk_%: test_gbk_%.c test_common.h $(MOD_OBJS) \
                     $(MODDIR)/gbk.h ../hw/h3hw.h
	$(CC) $(TESTCFLAGS) -o $@ $< $(MOD_OBJS)

run: $(TEST_BINS) ilp32gate
	@fail=0; \
	for m in $(MODULES); do \
	    if ! ./$(BUILD)/test_gbk_$$m; then fail=1; fi; \
	done; \
	if [ $$fail -eq 0 ]; then \
	    echo "gbkernel-test: ALL PASS ($(words $(MODULES))/$(words $(MODULES)) modules)"; \
	else \
	    echo "gbkernel-test: FAIL"; exit 1; \
	fi

clean:
	rm -rf $(BUILD)
