CC ?= cc
PKGS := xcb xcb-keysyms cairo-xcb imlib2
CFLAGS += -std=c99 -Wall -Wextra -O2 -g -I../common -I../wm/src -I../bar $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

# Reuses the WM's imlib2 image loader for image-file thumbnails and the bar's
# popup menu for the right-click New Folder/Rename/Delete context menu.
SRC := nekos-files.c ../wm/src/image.c ../bar/menu.c
BIN := nekos-files

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)
