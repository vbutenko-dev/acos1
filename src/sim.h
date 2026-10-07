#ifndef SIM_H
#define SIM_H

// Модель обедающих философов: стол, философы, вилки и их поведение.

#include <signal.h>
#include <stdbool.h>
#include "event.h"
#include "rng.h"

// Стратегии взятия вилок
#define STRATEGY_ORDERED 0  // сначала вилка с меньшим номером, потом с большим
#define STRATEGY_NAIVE   1  // сначала левая, потом правая (может зависнуть)

// Параметры модели
typedef struct Config {
    int n;                  // число философов (и вилок)
    int cycles;             // сколько раз каждый должен поесть, 0 = без ограничения
    long long thinkMin;     // время размышления от .. до
    long long thinkMax;
    long long eatMin;       // время еды от .. до
    long long eatMax;
    int strategy;           // STRATEGY_ORDERED или STRATEGY_NAIVE
    long long maxWait;      // допустимое время ожидания вилок
    long long timeLimit;    // ограничение модельного времени, 0 = нет
    unsigned long long seed;
    int delay;              // пауза после каждого события, мс
} Config;

// Состояния философа
enum { THINKING, HUNGRY, EATING, DONE };

typedef struct Fork {
    int owner;      // кто держит вилку, -1 = свободна
    int waiter;     // кто ждёт вилку, -1 = никто (ждать может только второй сосед)
} Fork;

typedef struct Philosopher {
    int state;
    int meals;              // сколько раз поел
    long long hungrySince;  // когда проголодался
    long long totalWait;    // сколько всего ждал вилки
    long long longestWait;  // самое долгое ожидание
    int warnings;           // сколько раз ждал дольше maxWait
} Philosopher;

typedef struct Table {
    Config cfg;
    Philosopher* phils;
    Fork* forks;
    EventQueue events;
    Rng rng;
    long long now;          // текущее модельное время
    int doneCount;          // сколько философов уже поели нужное число раз
} Table;

// Чем закончилось моделирование
enum { RESULT_DONE, RESULT_TIME_LIMIT, RESULT_INTERRUPTED, RESULT_DEADLOCK, RESULT_ERROR };

// Номер сигнала (Ctrl+C), 0 = сигнала не было. Ставится в main.c.
extern volatile sig_atomic_t stopSignal;

// Вывод сразу на экран и в лог-файл
void SetLogFile(int fd);
void Print(const char* format, ...);

bool TableInit(Table* t, const Config* cfg);
void TableFree(Table* t);

// Запуск моделирования, возвращает RESULT_...
int SimRun(Table* t);

// Вывод состояния стола и итоговой статистики
void PrintState(Table* t);
void PrintStats(Table* t);

#endif
