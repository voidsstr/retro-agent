# Build everything DDK-independent in the 3dfx project + run all host tests.
#   make        - glide DLLs + gfxbench.exe + fxD3D core + driver glue + tests
#   make glide  - just the open Glide DLLs (build-glide.sh)
#   make bench  - just gfxbench.exe (CARD=h5|h3)
#   make test   - just the host unit tests (d3dhal + driver glue)
#   make clean

CARD ?= h5

all: glide bench core test
	@echo "== 3dfx build complete =="

glide:
	./build-glide.sh

bench:
	$(MAKE) -C gfxbench CARD=$(CARD)

core:
	$(MAKE) -C d3dhal winobj
	$(MAKE) -C driver winobj

test:
	$(MAKE) -C d3dhal test
	$(MAKE) -C driver test

clean:
	-$(MAKE) -C gfxbench clean
	-$(MAKE) -C d3dhal clean
	-$(MAKE) -C driver clean
	rm -rf out build

.PHONY: all glide bench core test clean
