#include "net.h"
#include "rng.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint64_t low, high;
} Interval;

static int Range(const char *text, Interval *interval) {
    char copy[64];
    if (strlen(text) >= sizeof(copy)) return 0;
    strcpy(copy, text);
    char *dash = strchr(copy, '-');
    if (dash) *dash++ = '\0';
    return Number(copy, MAX_TIME_MS, &interval->low) &&
           Number(dash ? dash : copy, MAX_TIME_MS, &interval->high) &&
           interval->low <= interval->high;
}

static void Help(const char *program) {
    dprintf(1, "Использование: %s IP PORT ID [-t MIN-MAX] [-e MIN-MAX] [-r SEED]\n"
            "  ID — номер философа от 0 до N-1\n"
            "  -t — время размышления в мс (100-500)\n"
            "  -e — время еды в мс (200-400)\n"
            "  -r — начальное значение генератора (42)\n"
            "Вместо диапазона допустимо одно число от 0 до 86400000.\n", program);
}

static int Run(int fd, int id, Interval think, Interval eat, uint64_t seed) {
    Rng rng;
    RngSeed(&rng, seed + (uint64_t)id);
    while (1) {
        unsigned char command;
        ssize_t result = read(fd, &command, 1);
        if (result < 0 && errno == EINTR) continue;
        if (result != 1) break;
        if (command == FINISH) {
            dprintf(1, "Философ %d: выполнил все циклы\n", id);
            return 0;
        }

        Interval interval;
        const char *action, *after;
        unsigned char reply;
        if (command == THINK) {
            interval = think;
            action = "размышляет";
            after = "проголодался, просит вилки";
            reply = HUNGRY;
        } else if (command == EAT) {
            interval = eat;
            action = "получил вилки, ест";
            after = "закончил есть, возвращает вилки";
            reply = RELEASE;
        } else {
            return 1;
        }

        int ms = (int)RngRange(&rng, interval.low, interval.high);
        dprintf(1, "Философ %d: %s (%d мс)\n", id, action, ms);
        // Во время паузы poll также заметит закрытие соединения.
        struct pollfd connection = {fd, POLLIN, 0};
        if (poll(&connection, 1, ms) != 0) return 1;
        dprintf(1, "Философ %d: %s\n", id, after);
        if (!SendByte(fd, reply)) return 1;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        Help(argv[0]);
        return 0;
    }
    uint64_t id;
    if (argc < 4 || !Number(argv[3], MAX_PHILOSOPHERS - 1, &id)) {
        Help(argv[0]);
        return 2;
    }
    Interval think = {100, 500}, eat = {200, 400};
    uint64_t seed = 42;
    int option, valid = 1;
    optind = 4;
    while ((option = getopt(argc, argv, "t:e:r:")) != -1) {
        switch (option) {
            case 't': valid = Range(optarg, &think); break;
            case 'e': valid = Range(optarg, &eat); break;
            case 'r': valid = Number(optarg, UINT64_MAX, &seed); break;
            default: valid = 0; break;
        }
        if (!valid) break;
    }
    if (!valid || optind != argc) {
        Help(argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    int fd = Connect(argv[1], argv[2]);
    if (fd < 0) {
        perror("connect");
        return 1;
    }
    int result = 1;
    if (SendByte(fd, (unsigned char)(id + 1))) {
        result = Run(fd, (int)id, think, eat, seed);
    }
    if (result) dprintf(2, "Философ %d: соединение со столом завершено досрочно\n", (int)id);
    close(fd);
    return result;
}
