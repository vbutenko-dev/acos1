#ifndef HW2_SIM_H
#define HW2_SIM_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

enum { STRATEGY_ORDERED, STRATEGY_NAIVE };

typedef struct Config {
    int n;
    int cycles;                 // 0: работать до сигнала или ограничения времени
    long long thinkMin;         // все интервалы измеряются в реальных миллисекундах
    long long thinkMax;
    long long eatMin;
    long long eatMax;
    int strategy;
    long long maxWait;
    long long timeLimit;        // 0: без ограничения
    uint64_t seed;
    int delay;                  // дополнительная пауза вывода, вне мьютекса
    bool syncFirstFork;         // демонстрация naive: дождаться первых вилок у всех
} Config;

extern volatile sig_atomic_t stopSignal;

// Запускает потоки философов, ждёт их завершения и печатает статистику.
// Коды: 0 — успех, 1 — ошибка запуска, 3 — взаимоблокировка, 4 — ошибка модели,
// 5 — время истекло до выполнения всех циклов, 128 + сигнал — прерывание.
int RunSimulation(const Config *cfg, int logFd);

#endif
