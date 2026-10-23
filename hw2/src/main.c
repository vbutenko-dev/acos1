#include "sim.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MAX_TIME_MS = 86400000, MAX_DELAY_MS = 60000 };
enum ParseResult { PARSE_OK, PARSE_HELP, PARSE_ERROR };

static void OnSignal(int signalNumber) {
    stopSignal = signalNumber;
}

static void PrintHelp(const char *program) {
    dprintf(STDOUT_FILENO,
            "Использование: %s [параметры]\n"
            "  -n N        число философов, от 2 до 256 (5)\n"
            "  -c N        число приёмов пищи, 0 — без ограничения (3)\n"
            "  -t MIN-MAX  размышление в реальных мс, можно одно число (100-500)\n"
            "  -e MIN-MAX  еда в реальных мс, можно одно число (200-400)\n"
            "  -s NAME     стратегия ordered или naive (ordered)\n"
            "  -w N        допустимое ожидание в мс, от 1 до 86400000 (2000)\n"
            "  -T N        ограничение реального времени в мс, 0 — нет (0)\n"
            "  -r N        seed от 0 до 18446744073709551615 (42)\n"
            "  -d N        дополнительная пауза вывода в мс, до 60000 (0)\n"
            "  -l FILE     лог-файл (philosophers-hw2.log)\n"
            "  -b          барьер первых вилок для демонстрации deadlock в naive\n"
            "  -h          эта справка\n"
            "Интервалы -t, -e и -T: от 0 до 86400000 мс.\n",
            program);
}

// Не принимаем знак и пробелы. Переполнение проверяет strtoull.
static bool ParseUnsigned(const char *text, uint64_t *value, const char **end) {
    if (*text < '0' || *text > '9') {
        return false;
    }
    char *tail;
    errno = 0;
    unsigned long long number = strtoull(text, &tail, 10);
    if (errno != 0) {
        return false;
    }
    *value = (uint64_t)number;
    *end = tail;
    return true;
}

static bool ParseNumber(const char *text, uint64_t minimum, uint64_t maximum,
                        uint64_t *value) {
    const char *end;
    uint64_t result;
    if (!ParseUnsigned(text, &result, &end) || *end != '\0' ||
        result < minimum || result > maximum) {
        return false;
    }
    *value = result;
    return true;
}

static bool ParseRange(const char *text, long long *minimum, long long *maximum) {
    const char *end;
    uint64_t low;
    uint64_t high;
    if (!ParseUnsigned(text, &low, &end)) {
        return false;
    }
    if (*end == '\0') {
        high = low;
    } else if (*end == '-') {
        if (!ParseUnsigned(end + 1, &high, &end) || *end != '\0') {
            return false;
        }
    } else {
        return false;
    }
    if (low > high || high > MAX_TIME_MS) {
        return false;
    }
    *minimum = (long long)low;
    *maximum = (long long)high;
    return true;
}

static enum ParseResult ParseArgs(Config *config, const char **logPath,
                                  int argc, char **argv) {
    int option;
    opterr = 0;
    while ((option = getopt(argc, argv, "n:c:t:e:s:w:T:r:d:l:bh")) != -1) {
        bool valid = true;
        uint64_t number;
        switch (option) {
            case 'n':
                valid = ParseNumber(optarg, 2, 256, &number);
                if (valid) {
                    config->n = (int)number;
                }
                break;
            case 'c':
                valid = ParseNumber(optarg, 0, INT_MAX, &number);
                if (valid) {
                    config->cycles = (int)number;
                }
                break;
            case 't':
                valid = ParseRange(optarg, &config->thinkMin, &config->thinkMax);
                break;
            case 'e':
                valid = ParseRange(optarg, &config->eatMin, &config->eatMax);
                break;
            case 's':
                if (strcmp(optarg, "ordered") == 0) {
                    config->strategy = STRATEGY_ORDERED;
                } else if (strcmp(optarg, "naive") == 0) {
                    config->strategy = STRATEGY_NAIVE;
                } else {
                    valid = false;
                }
                break;
            case 'w':
                valid = ParseNumber(optarg, 1, MAX_TIME_MS, &number);
                if (valid) {
                    config->maxWait = (long long)number;
                }
                break;
            case 'T':
                valid = ParseNumber(optarg, 0, MAX_TIME_MS, &number);
                if (valid) {
                    config->timeLimit = (long long)number;
                }
                break;
            case 'r':
                valid = ParseNumber(optarg, 0, UINT64_MAX, &config->seed);
                break;
            case 'd':
                valid = ParseNumber(optarg, 0, MAX_DELAY_MS, &number);
                if (valid) {
                    config->delay = (int)number;
                }
                break;
            case 'l':
                valid = *optarg != '\0';
                if (valid) {
                    *logPath = optarg;
                }
                break;
            case 'b':
                config->syncFirstFork = true;
                break;
            case 'h':
                return PARSE_HELP;
            default:
                dprintf(STDERR_FILENO, "Неизвестный параметр или нет значения: -%c\n",
                        optopt);
                return PARSE_ERROR;
        }
        if (!valid) {
            dprintf(STDERR_FILENO, "Неверное значение параметра -%c: %s\n",
                    option, optarg);
            return PARSE_ERROR;
        }
    }
    if (optind < argc) {
        dprintf(STDERR_FILENO, "Лишний аргумент: %s\n", argv[optind]);
        return PARSE_ERROR;
    }
    if (config->syncFirstFork && config->strategy != STRATEGY_NAIVE) {
        dprintf(STDERR_FILENO, "Параметр -b применяется только со стратегией naive.\n");
        return PARSE_ERROR;
    }
    return PARSE_OK;
}

int main(int argc, char **argv) {
    Config config = {
        .n = 5,
        .cycles = 3,
        .thinkMin = 100,
        .thinkMax = 500,
        .eatMin = 200,
        .eatMax = 400,
        .strategy = STRATEGY_ORDERED,
        .maxWait = 2000,
        .timeLimit = 0,
        .seed = 42,
        .delay = 0,
        .syncFirstFork = false,
    };
    const char *logPath = "philosophers-hw2.log";
    enum ParseResult parsed = ParseArgs(&config, &logPath, argc, argv);
    if (parsed == PARSE_HELP) {
        PrintHelp(argv[0]);
        return 0;
    }
    if (parsed == PARSE_ERROR) {
        dprintf(STDERR_FILENO, "Справка: %s -h\n", argv[0]);
        return 2;
    }

    int logFd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logFd < 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть лог %s: %s\n",
                logPath, strerror(errno));
        return 1;
    }

    struct sigaction action = {0};
    action.sa_handler = OnSignal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        dprintf(STDERR_FILENO, "Не удалось установить обработчик сигнала: %s\n",
                strerror(errno));
        close(logFd);
        return 1;
    }

    int result = RunSimulation(&config, logFd);
    if (close(logFd) < 0) {
        dprintf(STDERR_FILENO, "Ошибка закрытия лога: %s\n", strerror(errno));
        if (result == 0) {
            result = 1;
        }
    }
    return result;
}
