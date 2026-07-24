CC ?= cc
PKGS := xcb xcb-randr cairo-xcb imlib2
CFLAGS += -std=c99 -Wall -Wextra -O2 -g -I../common -I../wm/src -I../bar $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

# Reuses the WM's imlib2 wallpaper loader and the bar's popup menu.
SRC := nekos-desktop.c ../wm/src/image.c ../bar/menu.c
BIN := nekos-desktop

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)
