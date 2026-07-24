CC ?= cc
PKGS := xcb xcb-keysyms cairo-xcb
CFLAGS += -std=c99 -Wall -Wextra -O2 -g -I../common $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

SRC := nekos-shot.c
BIN := nekos-shot

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)
