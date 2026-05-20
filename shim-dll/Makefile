CC ?= i686-w64-mingw32-gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2 -m32 -Iinclude
LDFLAGS ?= -shared

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
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(DDRAW_DEF) -Wl,--enable-stdcall-fixup

$(DINPUT_OUT): $(DINPUT_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(DINPUT_DEF) -Wl,--enable-stdcall-fixup

# Debug builds with logging
debug-ddraw: $(DDRAW_SRC)
	$(CC) $(CFLAGS) -DSHIM_DEBUG -o $(DDRAW_OUT) $^ $(LDFLAGS) $(DDRAW_DEF) -Wl,--enable-stdcall-fixup

debug-dinput: $(DINPUT_SRC)
	$(CC) $(CFLAGS) -DSHIM_DEBUG -o $(DINPUT_OUT) $^ $(LDFLAGS) $(DINPUT_DEF) -Wl,--enable-stdcall-fixup

clean:
	rm -f $(DDRAW_OUT) $(DINPUT_OUT)
