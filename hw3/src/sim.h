#ifndef HW3_SIM_H
#define HW3_SIM_H

#include <stdint.h>

enum { MAX_PHILOSOPHERS = 64, MAX_TIME_MS = 86400000 };

typedef struct Config {
    int n;
    int cycles;                 // 0 — до сигнала или ограничения времени
    long long thinkMin, thinkMax;
    long long eatMin, eatMax;
    long long maxWait, timeLimit;
    uint64_t seed;
} Config;

// В fork-версиях ключ создаётся по PID родителя.
int RunTable(const Config *cfg, const char *key, const char *logPath,
             int independent, int named);
int RunPhilosopher(const char *key, int id);
int InstallSignals(void);

#endif
