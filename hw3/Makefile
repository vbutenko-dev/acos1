CC = cc
CFLAGS = -std=c17 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -O2
SRC = src/main.c src/sim.c src/rng.c
HDR = src/sim.h src/rng.h
LDLIBS = -pthread -lrt

all: bin/philosophers-named bin/philosophers-unnamed bin/table bin/philosopher

bin/philosophers-named: $(SRC) $(HDR) | bin
	$(CC) $(CFLAGS) $(SRC) $(LDLIBS) -o $@

bin/philosophers-unnamed: $(SRC) $(HDR) | bin
	$(CC) $(CFLAGS) -DUNNAMED $(SRC) $(LDLIBS) -o $@

bin/table: $(SRC) $(HDR) | bin
	$(CC) $(CFLAGS) -DINDEPENDENT $(SRC) $(LDLIBS) -o $@

bin/philosopher: $(SRC) $(HDR) | bin
	$(CC) $(CFLAGS) -DWORKER $(SRC) $(LDLIBS) -o $@

bin:
	mkdir -p $@

test: all
	python3 tests/run_tests.py

clean:
	rm -rf bin tests/out

.PHONY: all test clean
