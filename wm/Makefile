CC ?= cc
PKGS := xcb xcb-composite xcb-damage xcb-render xcb-renderutil xcb-xfixes xcb-randr xcb-cursor cairo-xcb xcb-keysyms imlib2
CFLAGS += -std=c99 -Wall -Wextra -O2 -g -I../common $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm

SRC := src/main.c src/atoms.c src/compositor.c src/anim.c src/client.c src/decor.c src/ewmh.c src/keys.c src/image.c
BIN := nekos-wm

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)
