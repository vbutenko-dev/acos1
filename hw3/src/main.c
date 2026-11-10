#include "sim.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int Number(const char *text, uint64_t max, uint64_t *value) {
    if (*text < '0' || *text > '9') return 0;
    char *end;
    errno = 0;
    unsigned long long result = strtoull(text, &end, 10);
    if (errno || *end || result > max) return 0;
    *value = result;
    return 1;
}

#ifndef WORKER
static int Range(const char *text, long long *low, long long *high) {
    char copy[64];
    if (strlen(text) >= sizeof(copy)) return 0;
    strcpy(copy, text);
    char *dash = strchr(copy, '-');
    if (dash) *dash++ = '\0';
    uint64_t a, b;
    if (!Number(copy, MAX_TIME_MS, &a) ||
        !Number(dash ? dash : copy, MAX_TIME_MS, &b) || a > b) return 0;
    *low = (long long)a;
    *high = (long long)b;
    return 1;
}

static void Help(const char *program) {
    dprintf(1, "Использование: %s [параметры]\n"
            "  -n N        философов и вилок: 2..64 (5)\n"
            "  -c N        приёмов пищи у каждого, 0 — без ограничения (3)\n"
            "  -t MIN-MAX  время размышления в мс (100-500)\n"
            "  -e MIN-MAX  время еды в мс (200-400)\n"
            "  -s fifo     выдача обеих вилок по очереди голодных\n"
            "  -w MS       максимальное ожидание вилок (2000)\n"
            "  -T MS       предел времени, 0 — нет (0)\n"
            "  -r SEED     начальное значение генератора (42)\n"
            "  -l FILE     журнал (philosophers-hw3.log)\n"
#ifdef INDEPENDENT
            "  -k KEY      имя стола (demo), до 20 ASCII-букв/цифр/_/-\n"
#endif
            "  -h          справка\n"
            "Вместо диапазона можно одно число. Время: 0..86400000 мс, -w > 0.\n",
            program);
}
#endif

int main(int argc, char **argv) {
    if (InstallSignals() != 0) {
        perror("sigaction");
        return 1;
    }
#ifdef WORKER
    uint64_t id;
    if (argc != 3 || !Number(argv[2], MAX_PHILOSOPHERS - 1, &id)) {
        dprintf(2, "Использование: %s KEY ID (ID от 0 до N-1)\n", argv[0]);
        return 2;
    }
    return RunPhilosopher(argv[1], (int)id);
#else
    Config cfg = {5, 3, 100, 500, 200, 400, 2000, 0, 42};
    const char *key = "demo", *logPath = "philosophers-hw3.log";
    int option;
    opterr = 0;
    while ((option = getopt(argc, argv, "n:c:t:e:s:w:T:r:l:k:h")) != -1) {
        uint64_t value = 0;
        int valid = 1;
        switch (option) {
            case 'n':
                valid = Number(optarg, MAX_PHILOSOPHERS, &value) && value >= 2;
                cfg.n = (int)value;
                break;
            case 'c':
                valid = Number(optarg, INT_MAX, &value);
                cfg.cycles = (int)value;
                break;
            case 't': valid = Range(optarg, &cfg.thinkMin, &cfg.thinkMax); break;
            case 'e': valid = Range(optarg, &cfg.eatMin, &cfg.eatMax); break;
            case 's': valid = strcmp(optarg, "fifo") == 0; break;
            case 'w':
                valid = Number(optarg, MAX_TIME_MS, &value) && value > 0;
                cfg.maxWait = (long long)value;
                break;
            case 'T':
                valid = Number(optarg, MAX_TIME_MS, &value);
                cfg.timeLimit = (long long)value;
                break;
            case 'r': valid = Number(optarg, UINT64_MAX, &cfg.seed); break;
            case 'l': logPath = optarg; valid = *optarg != '\0'; break;
            case 'k':
#ifdef INDEPENDENT
                key = optarg;
#else
                valid = 0;
#endif
                break;
            case 'h': Help(argv[0]); return 0;
            default: valid = 0;
        }
        if (!valid) {
            dprintf(2, "Неверный параметр -%c. Справка: %s -h\n", option, argv[0]);
            return 2;
        }
    }
    if (optind != argc) {
        dprintf(2, "Лишние аргументы. Справка: %s -h\n", argv[0]);
        return 2;
    }
#ifdef INDEPENDENT
    return RunTable(&cfg, key, logPath, 1, 1);
#else
    (void)key;
    char processKey[24];
    snprintf(processKey, sizeof(processKey), "%ld", (long)getpid());
#ifdef UNNAMED
    return RunTable(&cfg, processKey, logPath, 0, 0);
#else
    return RunTable(&cfg, processKey, logPath, 0, 1);
#endif
#endif
#endif
}
