# Stranglehold native: build the runtime and the C generated from the user's own game files.
#   make GAME=<the installed game's folder (Binaries/, StrangleholdGame/)>
# Python with Capstone: the repository's .venv
PY ?= $(firstword $(wildcard .venv/bin/python) $(wildcard $(HOME)/DevWork/larush-native/.venv/bin/python) python3)
GAME ?= $(HOME)/.local/share/stranglehold-native/game
BUILD_CFG ?= builds/gog.py
# Parallel compiles: each takes ~300 MB; keep it modest so the laptop stays usable.
JOBS ?= 6
MAKEFLAGS += -j$(JOBS)
CC = gcc
CXX = g++
SDL3 ?= third_party/sdl3
DXVK ?= third_party/dxvk-native
CFLAGS_COMMON = -std=gnu11 -g -fno-strict-aliasing -frounding-math -fno-omit-frame-pointer -Iruntime -Igen -I$(SDL3)/include
CFLAGS_GEN = $(CFLAGS_COMMON) -O2 -w -DCPU_FAST_MEM
CFLAGS_RT = $(CFLAGS_COMMON) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
LDFLAGS = -rdynamic -L$(SDL3)/lib -Wl,-rpath,'$$ORIGIN/lib' -lSDL3 -pthread -lm -ldl -lz

RT_SRC = $(wildcard runtime/*.c) gen/d3d9_gen.c
DXVK_INC = -I$(DXVK)/include/dxvk
# the decompiled functions (runtime/decomp/, ROADMAP.md)
DC_SRC = $(wildcard runtime/decomp/*.c)
RT_OBJ = $(patsubst runtime/%.c,build/obj/rt_%.o,$(filter runtime/%,$(RT_SRC))) build/obj/rt_d3d9_gen.o \
         build/obj/rt_shaders_gen.o \
         $(patsubst runtime/decomp/%.c,build/obj/dc_%.o,$(DC_SRC))
# The code files recomp.py wrote, listed in gen/files.mk once it has run.
-include gen/files.mk
GEN_SRC = $(GEN_CODE) gen/table.c gen/stubs.c
GEN_OBJ = $(patsubst gen/%.c,build/obj/gen_%.o,$(GEN_SRC))

all: build/stranglehold build/lib/libSDL3.so.0 build/lib/libdxvk_d3d9.so.0

# (the translator's own scripts: other tools/ changes don't regenerate)
RECOMP_PY = $(wildcard $(addprefix tools/,recomp.py codegen.py discover.py image.py pe.py fpustack.py simd.py x86.py))
gen/funcs.h: $(RECOMP_PY) $(BUILD_CFG)
	$(PY) tools/recomp.py $(GAME) $(BUILD_CFG) gen
gen/table.c gen/stubs.c: gen/funcs.h ;
gen/files.mk: gen/funcs.h
	@echo 'GEN_CODE = $(sort $(wildcard gen/code_*.c))' > $@

build/obj/gen_%.o: gen/%.c gen/funcs.h runtime/cpu.h | build/obj
	$(CC) $(CFLAGS_GEN) -c $< -o $@

build/obj/rt_%.o: runtime/%.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@

# (-ffp-contract=off: no fused multiply-adds, so each operation rounds as the original's does)
build/obj/dc_%.o: runtime/decomp/%.c runtime/decomp/*.h runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -ffp-contract=off -c $< -o $@

# the Direct3D 9 bridge: DXVK's headers for the types, wrappers generated from its d3d9.h
gen/d3d9_gen.c gen/d3d9_gen.h: tools/d3d9gen.py $(DXVK)/include/dxvk/d3d9.h
	$(PY) tools/d3d9gen.py $(DXVK)/include/dxvk/d3d9.h gen/d3d9_gen.c
build/obj/rt_d3d9.o: runtime/d3d9.c runtime/*.h gen/d3d9_gen.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) $(DXVK_INC) -Wno-unused-function -c $< -o $@
build/obj/rt_d3d9_%.o: runtime/d3d9_%.c runtime/*.h gen/d3d9_gen.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) $(DXVK_INC) -c $< -o $@
build/obj/rt_d3d9_gen.o: gen/d3d9_gen.c runtime/*.h gen/d3d9_gen.h | build/obj
	$(CC) $(CFLAGS_RT) $(DXVK_INC) -Wno-unused-function -c $< -o $@

# replacement shaders (option shaders = improved, runtime/d3d9.c): data/shaders/<hash>.vsh/.psh,
# shader assembly, assembled into a C table. tools/d3dasm.py runs on every make (a fraction of a
# second) and rewrites the table only when it changes, so adding or removing a file is noticed too.
gen/shaders_gen.c: FORCE
	$(PY) tools/d3dasm.py ctable data/shaders $@
build/obj/rt_shaders_gen.o: gen/shaders_gen.c | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@
FORCE:

build/stranglehold: $(RT_OBJ) $(GEN_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# the shared libraries the program loads, beside it
build/lib/libSDL3.so.0: $(SDL3)/lib/libSDL3.so.0 | build/obj
	mkdir -p build/lib && cp -L $< $@
build/lib/libdxvk_d3d9.so.0: $(DXVK)/lib/x86_64-linux-gnu/libdxvk_d3d9.so.0 | build/obj
	mkdir -p build/lib && cp -L $< $@

# make install: the game in ~/.local/opt/stranglehold-native (the program and its libraries) with an
# applications-menu entry and the game's own icon (taken from the user's LARush.exe)
PREFIX ?= $(HOME)/.local/opt/stranglehold-native
APPS ?= $(HOME)/.local/share/applications
install: all
	mkdir -p $(PREFIX)/lib $(APPS)
	cp build/stranglehold $(PREFIX)/stranglehold.new && mv -f $(PREFIX)/stranglehold.new $(PREFIX)/stranglehold
	cp -L build/lib/libSDL3.so.0 build/lib/libdxvk_d3d9.so.0 $(PREFIX)/lib/
	-python3 tools/icon.py $(GAME)/Binaries/Retail-Stranglehold.exe $(PREFIX)/stranglehold.png
	printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=Stranglehold' \
		'Comment=Stranglehold recompiled for Linux and Vulkan' \
		'Exec=$(PREFIX)/stranglehold' 'Icon=$(PREFIX)/stranglehold.png' 'Categories=Game;' \
		'StartupWMClass=stranglehold-native' 'Terminal=false' > $(APPS)/stranglehold-native.desktop
	@echo "installed in $(PREFIX); the applications menu has L.A. Rush"

# Function-level check against the original code in Unicorn (tests/fncheck.c)
UNICORN ?= $(firstword $(wildcard .venv/lib/python3*/site-packages/unicorn))
build/obj/rt_main_nomain.o: runtime/main.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -DRUNTIME_NO_MAIN -c $< -o $@
build/obj/gen_x87_special.o: gen/x87_special.c | build/obj
	$(CC) $(CFLAGS_GEN) -c $< -o $@
build/fncheck: tests/fncheck.c $(filter-out build/obj/rt_main.o,$(RT_OBJ)) build/obj/rt_main_nomain.o $(GEN_OBJ) build/obj/gen_x87_special.o
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

build/obj:
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all clean install FORCE
