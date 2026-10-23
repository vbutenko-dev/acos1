CC = cc
CFLAGS = -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -g
SRC = src/main.c src/sim.c src/rng.c
HDR = src/sim.h src/rng.h

all: bin/philosophers-sem bin/philosophers-cond

bin/philosophers-sem: $(SRC) $(HDR) | bin
	$(CC) $(CFLAGS) -pthread -o $@ $(SRC)

bin/philosophers-cond: $(SRC) $(HDR) | bin
	$(CC) $(CFLAGS) -pthread -DUSE_CONDVAR -o $@ $(SRC)

bin:
	mkdir -p $@

test: all
	python3 tests/run_tests.py

clean:
	rm -rf bin tests/out

.PHONY: all test clean
