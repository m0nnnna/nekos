CC ?= cc
PKGS := xcb xcb-keysyms cairo-xcb imlib2
CFLAGS += -std=c99 -Wall -Wextra -O2 -g -I../common -I../wm/src $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

# Reuses the WM's imlib2 image loader for .desktop app icons.
SRC := nekos-launch.c ../wm/src/image.c
BIN := nekos-launch

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)
