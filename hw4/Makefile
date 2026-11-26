CC = cc
CFLAGS = -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2

all: bin/table bin/philosopher bin/observer

bin/table: src/table.c src/net.c src/net.h | bin
	$(CC) $(CFLAGS) src/table.c src/net.c -o $@

bin/philosopher: src/philosopher.c src/net.c src/net.h src/rng.c src/rng.h | bin
	$(CC) $(CFLAGS) src/philosopher.c src/net.c src/rng.c -o $@

bin/observer: src/observer.c src/net.c src/net.h | bin
	$(CC) $(CFLAGS) src/observer.c src/net.c -o $@

bin:
	mkdir -p $@

test: all
	python3 tests/run_tests.py

clean:
	rm -rf bin

.PHONY: all test clean
