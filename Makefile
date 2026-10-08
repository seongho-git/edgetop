CC      ?= gcc
CFLAGS  ?= -O2
WARN    := -Wall -Wextra -Wconversion -Wshadow -Werror
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
STD     := -std=c11
LDLIBS  := -ldl

# system-wide for root, per-user otherwise; override with PREFIX=...
PREFIX  ?= $(if $(filter 0,$(shell id -u)),/usr/local,$(HOME)/.local)
BINDIR  := $(DESTDIR)$(PREFIX)/bin

SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:src/%.c=build/%.o)
LIBOBJ  := $(filter-out build/main.o,$(OBJ))

.PHONY: all install uninstall debug test test-one check check-footprint bench measure snap profile loadtest clean

all: edgetop

edgetop: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c src/*.h | build
	$(CC) $(STD) $(CFLAGS) $(WARN) $(CPPFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: edgetop
	install -d $(BINDIR)
	install -m 755 edgetop $(BINDIR)/edgetop

uninstall:
	rm -f $(BINDIR)/edgetop

debug: clean
	$(MAKE) CFLAGS="-O0 -g -fsanitize=address,undefined" LDLIBS="$(LDLIBS) -fsanitize=address,undefined"

build/run_tests: tests/test_main.c $(LIBOBJ) src/*.h | build
	$(CC) $(STD) $(CFLAGS) $(WARN) $(CPPFLAGS) -Isrc -o $@ tests/test_main.c $(LIBOBJ) $(LDLIBS)

test: build/run_tests
	cd tests && ../build/run_tests

test-one: build/run_tests
	cd tests && ../build/run_tests $(T)

check: edgetop
	python3 -I tools/check.py ./edgetop

check-footprint: edgetop
	sh tools/check_footprint.sh ./edgetop

bench: edgetop
	./edgetop --bench 2000
	./edgetop --bench 2000 --no-gpu

measure: edgetop
	python3 -I tools/measure_tui.py ./edgetop 30
	python3 -I tools/measure_tui.py "./edgetop --no-gpu" 30

snap: edgetop
	python3 -I tools/snap_tui.py ./edgetop

build/profile_sources: tools/profile_sources.c $(LIBOBJ) src/*.h | build
	$(CC) $(STD) $(CFLAGS) $(WARN) $(CPPFLAGS) -Isrc -o $@ tools/profile_sources.c $(LIBOBJ) $(LDLIBS)

profile: build/profile_sources
	./build/profile_sources

loadtest: edgetop
	python3 -I tools/loadtest.py ./edgetop

clean:
	rm -rf build edgetop
