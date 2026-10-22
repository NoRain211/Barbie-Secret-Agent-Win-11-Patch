# Recipes use cmd syntax, even when Git's sh.exe is on PATH.
SHELL = cmd.exe
CC = gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -m32 -DINITGUID -DSHIM_DEBUG -Iinclude
LDFLAGS ?= -shared
LDLIBS ?= -lole32 -luuid -lm

# ddraw proxy
DDRAW_OUT = ddraw.dll
DDRAW_SRC = src/main.c src/ddraw_proxy.c src/widescreen_fix.c src/menu_fix.c
DDRAW_DEF = ddraw.def

# dinput proxy (SDL3 gamepad support)
DINPUT_OUT = dinput.dll
DINPUT_SRC = src/dinput_main.c
DINPUT_DEF = dinput.def

TEST_DIR = build/tests
REGISTRY_PROBE = $(TEST_DIR)/registry_hook_probe.exe
DDRAW_ABI_PROBE = $(TEST_DIR)/ddraw_abi_probe.exe
DINPUT_LOGIC_PROBE = $(TEST_DIR)/dinput_logic_probe.exe
DINPUT_ABI_PROBE = $(TEST_DIR)/dinput_abi_probe.exe
WIDESCREEN_PROBE = $(TEST_DIR)/widescreen_probe.exe
MENU_FIX_PROBE = $(TEST_DIR)/menu_fix_probe.exe
GAME_EXE = ../game-files/SecretAgent.exe

all: $(DDRAW_OUT) $(DINPUT_OUT)

$(DDRAW_OUT): $(DDRAW_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(DDRAW_DEF) $(LDLIBS) -Wl,--enable-stdcall-fixup

$(DINPUT_OUT): $(DINPUT_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(DINPUT_DEF) $(LDLIBS) -Wl,--enable-stdcall-fixup

$(TEST_DIR):
	if not exist "$(TEST_DIR)" mkdir "$(TEST_DIR)"

$(REGISTRY_PROBE): tests/registry_hook_probe.c | $(TEST_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -O2 -m32 -o $@ $<

$(DDRAW_ABI_PROBE): tests/ddraw_abi_probe.c | $(TEST_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -O2 -m32 -o $@ $<

$(DINPUT_LOGIC_PROBE): $(DINPUT_SRC) | $(TEST_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -O2 -m32 -DINITGUID -DDINPUT_LOGIC_TEST \
		-Iinclude -o $@ $< $(LDLIBS)

$(DINPUT_ABI_PROBE): tests/dinput_abi_probe.c | $(TEST_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -O2 -m32 -o $@ $<

$(WIDESCREEN_PROBE): tests/widescreen_probe.c src/widescreen_fix.c | $(TEST_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -O2 -m32 -DINITGUID -DSHIM_DEBUG -DWIDESCREEN_TEST \
		-Iinclude -o $@ $^ $(LDLIBS)

$(MENU_FIX_PROBE): tests/menu_fix_probe.c src/menu_fix.c | $(TEST_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -O2 -m32 -DINITGUID -DMENU_FIX_TEST \
		-Iinclude -o $@ $^ $(LDLIBS)

test: all $(REGISTRY_PROBE) $(DDRAW_ABI_PROBE) $(DINPUT_LOGIC_PROBE) $(DINPUT_ABI_PROBE) $(WIDESCREEN_PROBE) $(MENU_FIX_PROBE)
	$(REGISTRY_PROBE) ./$(DDRAW_OUT)
	$(DDRAW_ABI_PROBE) ./$(DDRAW_OUT)
	$(DINPUT_LOGIC_PROBE)
	$(DINPUT_ABI_PROBE) ./$(DINPUT_OUT)
	$(MENU_FIX_PROBE)
	$(if $(wildcard $(GAME_EXE)),$(WIDESCREEN_PROBE) $(GAME_EXE),@echo Skipped widescreen probe: $(GAME_EXE) not found)

clean:
	-if exist $(DDRAW_OUT) del /q $(DDRAW_OUT)
	-if exist $(DINPUT_OUT) del /q $(DINPUT_OUT)
	-if exist build rmdir /s /q build

.PHONY: all test clean
