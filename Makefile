# Сборка программы "Обедающие философы" (вариант 5)

CC = cc
CFLAGS = -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g

SRC = src/main.c src/sim.c src/event.c src/rng.c
HDR = src/sim.h src/event.h src/rng.h

bin/philosophers: $(SRC) $(HDR)
	mkdir -p bin
	$(CC) $(CFLAGS) -o bin/philosophers $(SRC)

test: bin/philosophers
	bash tests/run_tests.sh

clean:
	rm -rf bin tests/out philosophers.log

.PHONY: test clean
