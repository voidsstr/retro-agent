# gfxbench - on-card 3dfx mode/option test + benchmark.
# Links the open Glide3 import lib built by ../build-glide.sh (run that first).
# Default target = Voodoo4/5 (h5); `make CARD=h3` for Voodoo3.

CROSS   ?= i686-w64-mingw32-
CC       = $(CROSS)gcc
CARD    ?= h5

SDK      = ../glide-sdk
BACKEND  = ../d3dhal
INC      = -I$(SDK)/include -I$(BACKEND)
LIB      = -L$(SDK)/lib -lglide3x_$(CARD)

CFLAGS   = -Os -Wall -DWIN32 -D__WIN32__ $(INC)
# Console subsystem so agent EXEC/LAUNCH sees stdout; it still opens fullscreen Glide.
# NOT -static: glide3x is a DLL (import lib), so the link must stay dynamic;
# -static-libgcc still folds in the gcc runtime so no extra DLLs are needed.
LDFLAGS  = -static-libgcc -Wl,--subsystem,console -luser32 -lgdi32 -lwinmm

OBJ = gfxbench.o glidebackend.o

all: gfxbench.exe

gfxbench.o: gfxbench.c $(BACKEND)/glidebackend.h
	$(CC) $(CFLAGS) -c -o $@ $<

glidebackend.o: $(BACKEND)/glidebackend.c $(BACKEND)/glidebackend.h
	$(CC) $(CFLAGS) -c -o $@ $<

gfxbench.exe: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LIB) $(LDFLAGS)
	@echo "built $@ (card=$(CARD)) - deploy next to glide3x.dll on the target"

clean:
	rm -f *.o gfxbench.exe
