# Top-level build: builds every nekOS component in place.
# Run inside the nekos-void distro from the repo root (~/nekos).
SUBDIRS = wm bar desktop launch notify settings files software pet editor shot

.PHONY: all clean $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done
