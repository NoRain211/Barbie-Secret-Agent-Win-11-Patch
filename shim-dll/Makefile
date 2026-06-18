CC = gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -m32 -DINITGUID -DSHIM_DEBUG -Iinclude
LDFLAGS ?= -shared
LDLIBS ?= -lole32 -luuid

# ddraw proxy
DDRAW_OUT = ddraw.dll
DDRAW_SRC = src/main.c src/ddraw_proxy.c
DDRAW_DEF = ddraw.def

# dinput proxy (XInput gamepad support)
DINPUT_OUT = dinput.dll
DINPUT_SRC = src/dinput_main.c
DINPUT_DEF = dinput.def

all: $(DDRAW_OUT) $(DINPUT_OUT)

$(DDRAW_OUT): $(DDRAW_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(DDRAW_DEF) $(LDLIBS) -Wl,--enable-stdcall-fixup

$(DINPUT_OUT): $(DINPUT_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(DINPUT_DEF) $(LDLIBS) -Wl,--enable-stdcall-fixup

# Explicit debug aliases. Distributed builds also keep logging enabled so
# support reports can confirm the shims loaded.
debug-ddraw: $(DDRAW_SRC)
	$(CC) $(CFLAGS) -o $(DDRAW_OUT) $^ $(LDFLAGS) $(DDRAW_DEF) $(LDLIBS) -Wl,--enable-stdcall-fixup

debug-dinput: $(DINPUT_SRC)
	$(CC) $(CFLAGS) -o $(DINPUT_OUT) $^ $(LDFLAGS) $(DINPUT_DEF) $(LDLIBS) -Wl,--enable-stdcall-fixup

clean:
	rm -f $(DDRAW_OUT) $(DINPUT_OUT)
