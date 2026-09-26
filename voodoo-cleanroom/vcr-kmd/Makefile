# vcr-kmd - our XP kernel-mode driver pair for 3dfx Banshee / Voodoo 3 / VSA-100.
#   make            -> out/vcrmp.sys (miniport) + out/vcrdd.dll (display) + out/vcrctl.exe
#   make check      -> host tests + import check against the XP SP3 export tables
# Toolchain: mingw-w64 i686 (gcc 13). Flags: docs/research-xp-driver-toolchain.md.
CROSS   ?= i686-w64-mingw32-
CC      := $(CROSS)gcc
DDKINC  ?= /usr/i686-w64-mingw32/include/ddk
BUILDNUM := $(shell cat .buildnum 2>/dev/null || echo 0)
OUT     := out

KCFLAGS := -O2 -march=i686 -mtune=pentium3 -ffreestanding -mgeneral-regs-only \
           -mno-stack-arg-probe -fno-stack-protector -fno-asynchronous-unwind-tables \
           -fno-common -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
           -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
           -D_X86_ -Di386 -DSTD_CALL -D_WIN32_WINNT=0x0501 -DVCR_KMD_BUILD=$(BUILDNUM) \
           -I$(DDKINC) -Icompat -Iinclude
KLDFLAGS := -nostdlib -nostartfiles -shared -Wl,--subsystem,native:5.01 \
            -Wl,--image-base,0x10000 -Wl,--exclude-all-symbols \
            -Wl,--disable-auto-import -Wl,--disable-stdcall-fixup

# the ring's sequence counter must be atomic in the kernel (lock xadd)
ATOMIC  := -D'VCR_LOG_NEXT_SEQ(p)=__sync_add_and_fetch((p),1u)'

MP_SRC  := miniport/vcrmp.c miniport/vcrmp_log.c miniport/vcrmp_hw.c miniport/vcrmp_map.c miniport/vcrmp_clock.c \
           miniport/vcrmp_multi.c miniport/vcrmp_sli.c miniport/vcrmp_ddc.c \
           common/vcr_log.c common/vcr_fmt.c common/vcr_modes.c common/vcr_ics307.c \
           common/vcr_edid.c
DD_SRC  := display/vcrdd.c display/vcrdd_escape.c display/vcrdd_log.c \
           common/vcr_fmt.c common/vcr_crt.c

all: $(OUT)/vcrmp.sys $(OUT)/vcrdd.dll $(OUT)/vcrctl.exe $(OUT)/vcrprobe.sys

$(OUT):
	mkdir -p $(OUT)

$(OUT)/vcrmp.sys: $(MP_SRC) $(wildcard include/*.h miniport/*.h) | $(OUT)
	$(CC) $(KCFLAGS) $(ATOMIC) -DVCR_SLI_HAVE_6K_CLOCK $(MP_SRC) -o $@ $(KLDFLAGS) \
	      -Wl,--entry,_DriverEntry@8 -lvideoprt -lntoskrnl -lhal -lgcc

$(OUT)/vcrdd.dll: $(DD_SRC) display/vcrdd.def $(wildcard include/*.h display/*.h) | $(OUT)
	$(CC) $(KCFLAGS) $(DD_SRC) display/vcrdd.def -o $@ $(KLDFLAGS) \
	      -Wl,--entry,_DrvEnableDriver@12 -lwin32k -lgcc

# vcrprobe.sys: a plain NT driver (not a miniport), loaded on the fly for captures
$(OUT)/vcrprobe.sys: probe/vcrprobe.c include/vcr_probe.h | $(OUT)
	# mingw's wdm.h uses a Vista-only type under the XP defines; declarations
	# only - check_imports.py still proves every import exists on XP SP3
	$(CC) $(KCFLAGS) -U_WIN32_WINNT -D_WIN32_WINNT=0x0600 probe/vcrprobe.c -o $@ $(KLDFLAGS) \
	      -Wl,--entry,_DriverEntry@8 -lntoskrnl -lhal -lgcc

$(OUT)/vcrctl.exe: tools/vcrctl.c $(wildcard include/*.h) | $(OUT)
	$(CC) -O2 -Wall -Wextra -march=i586 -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -Iinclude tools/vcrctl.c -o $@ -static -lgdi32 -luser32

imports: all
	python3 tools/check_imports.py $(OUT)/vcrmp.sys $(OUT)/vcrdd.dll $(OUT)/vcrprobe.sys

check: imports
	bash ../../tests/run_all.sh

clean:
	rm -rf $(OUT)

.PHONY: all imports check clean
