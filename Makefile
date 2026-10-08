CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS = -lX11 -lXfixes -lz
PREFIX ?= $(HOME)/.local

all: build/clipqueue
build/clipqueue: src/clipqueue.c src/sha256.c src/sha256.h src/png_hash.c src/png_hash.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/clipqueue.c src/sha256.c src/png_hash.c -o $@ $(LDFLAGS) $(LDLIBS)
install: all
	./install.sh "$(PREFIX)"
clean:
	rm -rf build
.PHONY: all install clean

build/libidentity.so: src/sha256.c src/png_hash.c src/sha256.h src/png_hash.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared src/sha256.c src/png_hash.c -o $@ $(LDFLAGS) -lz
test: build/libidentity.so
	python3 -m unittest discover -s tests/native -p 'test_*.py' -v
.PHONY: test
