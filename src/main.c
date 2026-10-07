// Главный файл задачи об обедающих философах (вариант 5)

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sim.h"

static void OnSignal(int sig) {
    stopSignal = sig;
}

static void PrintHelp(const char* prog) {
    dprintf(STDOUT_FILENO,
        "Использование: %s [параметры]\n"
        "  -n N        число философов, не меньше 2 (5)\n"
        "  -c N        сколько раз каждый должен поесть, 0 - без ограничения (3)\n"
        "  -t MIN-MAX  время размышления (100-500)\n"
        "  -e MIN-MAX  время еды (200-400)\n"
        "  -s NAME     стратегия: ordered или naive (ordered)\n"
        "  -w N        допустимое время ожидания (2000)\n"
        "  -T N        ограничение модельного времени, 0 - нет (0)\n"
        "  -r N        seed генератора случайных чисел (42)\n"
        "  -d N        пауза между событиями в мс (100)\n"
        "  -l FILE     лог-файл (philosophers.log)\n"
        "  -h          эта справка\n",
        prog);
}

// Чтение целого числа. Вся строка должна быть числом.
static bool ParseNumber(const char* s, long long* out) {
    char* end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

// Чтение диапазона "MIN-MAX" или одного числа "N"
static bool ParseRange(const char* s, long long* lo, long long* hi) {
    char buf[64];
    if (strlen(s) >= sizeof(buf)) {
        return false;
    }
    strcpy(buf, s);
    char* dash = strchr(buf, '-');
    if (dash == NULL) {
        if (!ParseNumber(buf, lo)) return false;
        *hi = *lo;
        return true;
    }
    *dash = '\0';
    return ParseNumber(buf, lo) && ParseNumber(dash + 1, hi);
}

static bool ParseArgs(Config* cfg, const char** logPath, int argc, char** argv) {
    long long v;
    int opt;
    opterr = 0; 
    while ((opt = getopt(argc, argv, "n:c:t:e:s:w:T:r:d:l:h")) != -1) {
        bool ok = true;
        switch (opt) {
            case 'n': ok = ParseNumber(optarg, &v); cfg->n = (int)v; break;
            case 'c': ok = ParseNumber(optarg, &v); cfg->cycles = (int)v; break;
            case 't': ok = ParseRange(optarg, &cfg->thinkMin, &cfg->thinkMax); break;
            case 'e': ok = ParseRange(optarg, &cfg->eatMin, &cfg->eatMax); break;
            case 'w': ok = ParseNumber(optarg, &cfg->maxWait); break;
            case 'T': ok = ParseNumber(optarg, &cfg->timeLimit); break;
            case 'r': ok = ParseNumber(optarg, &v); cfg->seed = (unsigned long long)v; break;
            case 'd': ok = ParseNumber(optarg, &v); cfg->delay = (int)v; break;
            case 'l': *logPath = optarg; break;
            case 's':
                if (strcmp(optarg, "ordered") == 0) cfg->strategy = STRATEGY_ORDERED;
                else if (strcmp(optarg, "naive") == 0) cfg->strategy = STRATEGY_NAIVE;
                else ok = false;
                break;
            case 'h':
                PrintHelp(argv[0]);
                exit(0);
            default:
                dprintf(STDERR_FILENO, "Неизвестный параметр или нет значения: -%c\n", optopt);
                return false;
        }
        if (!ok) {
            dprintf(STDERR_FILENO, "Неверное значение параметра -%c: %s\n", opt, optarg);
            return false;
        }
    }
    if (optind < argc) {
        dprintf(STDERR_FILENO, "Лишний аргумент: %s\n", argv[optind]);
        return false;
    }
    return true;
}

// Проверка параметров, возвращает текст ошибки или NULL
static const char* CheckConfig(const Config* cfg) {
    if (cfg->n < 2) return "философов должно быть не меньше 2";
    if (cfg->n > 1000) return "философов должно быть не больше 1000";
    if (cfg->cycles < 0) return "число циклов не может быть отрицательным";
    if (cfg->thinkMin < 0 || cfg->thinkMin > cfg->thinkMax) return "неверное время размышления";
    if (cfg->eatMin < 0 || cfg->eatMin > cfg->eatMax) return "неверное время еды";
    if (cfg->maxWait <= 0) return "допустимое время ожидания должно быть больше 0";
    if (cfg->timeLimit < 0) return "ограничение времени не может быть отрицательным";
    if (cfg->delay < 0) return "пауза не может быть отрицательной";
    return NULL;
}

int main(int argc, char** argv) {
    // значения по умолчанию
    Config cfg;
    cfg.n = 5;
    cfg.cycles = 3;
    cfg.thinkMin = 100;
    cfg.thinkMax = 500;
    cfg.eatMin = 200;
    cfg.eatMax = 400;
    cfg.strategy = STRATEGY_ORDERED;
    cfg.maxWait = 2000;
    cfg.timeLimit = 0;
    cfg.seed = 42;
    cfg.delay = 100;
    const char* logPath = "philosophers.log";

    if (!ParseArgs(&cfg, &logPath, argc, argv)) {
        dprintf(STDERR_FILENO, "Справка: %s -h\n", argv[0]);
        return 2;
    }
    const char* error = CheckConfig(&cfg);
    if (error != NULL) {
        dprintf(STDERR_FILENO, "Ошибка в параметрах: %s\n", error);
        return 2;
    }

    // лог-файл открываем системным вызовом open
    int logFd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logFd < 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть лог %s: %s\n", logPath, strerror(errno));
        return 1;
    }
    SetLogFile(logFd);

    // Ctrl+C и kill
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = OnSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    Table table;
    if (!TableInit(&table, &cfg)) {
        dprintf(STDERR_FILENO, "Не хватило памяти\n");
        close(logFd);
        return 1;
    }

    Print("Обедающие философы: %d философов, циклов %d, стратегия %s, seed %llu\n",
          cfg.n, cfg.cycles, cfg.strategy == STRATEGY_NAIVE ? "naive" : "ordered", cfg.seed);
    Print("Размышление %lld-%lld, еда %lld-%lld, допустимое ожидание %lld, ограничение времени %lld\n\n",
          cfg.thinkMin, cfg.thinkMax, cfg.eatMin, cfg.eatMax, cfg.maxWait, cfg.timeLimit);

    int result = SimRun(&table);

    int exitCode = 0;
    Print("\n");
    switch (result) {
        case RESULT_DONE:
            Print("Все философы поели нужное число раз.\n");
            exitCode = 0;
            break;
        case RESULT_TIME_LIMIT:
            if (cfg.cycles == 0) {
                Print("Моделирование закончено по времени.\n");
                exitCode = 0;
            } else {
                Print("Время вышло, а поели все циклы только %d философов из %d.\n",
                      table.doneCount, cfg.n);
                exitCode = 5;
            }
            PrintState(&table);
            break;
        case RESULT_INTERRUPTED:
            Print("Моделирование прервано (сигнал %d).\n", (int)stopSignal);
            PrintState(&table);
            exitCode = 128 + stopSignal;
            break;
        case RESULT_DEADLOCK:
            Print("Моделирование остановлено: взаимоблокировка.\n");
            PrintState(&table);
            exitCode = 3;
            break;
        case RESULT_ERROR:
            Print("Моделирование остановлено из-за ошибки.\n");
            PrintState(&table);
            exitCode = 4;
            break;
    }
    PrintStats(&table);
    Print("Код завершения: %d\n", exitCode);

    TableFree(&table);
    close(logFd);
    return exitCode;
}
