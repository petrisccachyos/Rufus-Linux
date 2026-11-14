CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wformat=2
GTK_FLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 json-glib-1.0)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0 json-glib-1.0)

.PHONY: all clean

all: rufus-linux

rufus-linux: rufus_linux.c ../resource.h
	$(CC) $(CFLAGS) $(GTK_FLAGS) -o $@ rufus_linux.c $(GTK_LIBS)

clean:
	rm -f rufus-linux
