CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS = -lX11 -lXfixes
PREFIX ?= $(HOME)/.local

all: build/clipqueue
build/clipqueue: src/clipqueue.c src/sha256.c src/sha256.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/clipqueue.c src/sha256.c -o $@ $(LDFLAGS) $(LDLIBS)
install: all
	./install.sh "$(PREFIX)"
clean:
	rm -rf build
.PHONY: all install clean
