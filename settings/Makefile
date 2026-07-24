CC ?= cc
PKGS := xcb cairo-xcb imlib2
CFLAGS += -std=c99 -Wall -Wextra -O2 -g -I../common -I../wm/src $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

# Reuses the WM's imlib2 image loader for wallpaper thumbnails.
SRC := nekos-settings.c ../wm/src/image.c
BIN := nekos-settings

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)
