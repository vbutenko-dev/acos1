#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "sim.h"

volatile sig_atomic_t stopSignal = 0;

static int logFd = -1;      // дескриптор лог-файла, -1 = лога нет

void SetLogFile(int fd) {
    logFd = fd;
}

// Печать на экран (дескриптор 1) и в лог
void Print(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vdprintf(STDOUT_FILENO, format, args);
    va_end(args);
    if (logFd >= 0) {
        va_start(args, format);
        vdprintf(logFd, format, args);
        va_end(args);
    }
}

// Строка о событии: "[t=   515] Философ 2: ..."
static void Say(Table* t, int p, const char* format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Print("[t=%6lld] Философ %d: %s\n", t->now, p, text);
}

static void Schedule(Table* t, long long time, EventType type, int p, int fork) {
    if (!EventQueuePush(&t->events, time, type, p, fork)) {
        dprintf(STDERR_FILENO, "Не хватило памяти\n");
        exit(1);
    }
}

// Пауза в миллисекундах, чтобы вывод можно было читать
static void Pause(int ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
// Стол

bool TableInit(Table* t, const Config* cfg) {
    t->cfg = *cfg;
    t->phils = calloc(cfg->n, sizeof(Philosopher));
    t->forks = calloc(cfg->n, sizeof(Fork));
    if (t->phils == NULL || t->forks == NULL) {
        free(t->phils);
        free(t->forks);
        return false;
    }
    for (int i = 0; i < cfg->n; i++) {
        t->forks[i].owner = -1;
        t->forks[i].waiter = -1;
    }
    EventQueueInit(&t->events);
    RngSeed(&t->rng, cfg->seed);
    t->now = 0;
    t->doneCount = 0;
    return true;
}

void TableFree(Table* t) {
    free(t->phils);
    free(t->forks);
    EventQueueFree(&t->events);
}

// У философа p левая вилка p, правая (p+1) по кругу
static int LeftFork(Table* t, int p)  { (void)t; return p; }
static int RightFork(Table* t, int p) { return (p + 1) % t->cfg.n; }

// Какую вилку брать первой и какую второй
static int FirstFork(Table* t, int p) {
    int l = LeftFork(t, p), r = RightFork(t, p);
    if (t->cfg.strategy == STRATEGY_NAIVE) {
        return l;
    }
    return (l < r) ? l : r;
}

static int SecondFork(Table* t, int p) {
    int l = LeftFork(t, p), r = RightFork(t, p);
    if (t->cfg.strategy == STRATEGY_NAIVE) {
        return r;
    }
    return (l < r) ? r : l;
}

static int ForksHeld(Table* t, int p) {
    int count = 0;
    if (t->forks[LeftFork(t, p)].owner == p) count++;
    if (t->forks[RightFork(t, p)].owner == p) count++;
    return count;
}

// Какую вилку ждёт философ, -1 = никакую
static int WaitingFor(Table* t, int p) {
    if (t->forks[LeftFork(t, p)].waiter == p) return LeftFork(t, p);
    if (t->forks[RightFork(t, p)].waiter == p) return RightFork(t, p);
    return -1;
}

static const char* StateName(int s) {
    switch (s) {
        case THINKING: return "думает";
        case HUNGRY:   return "голоден";
        case EATING:   return "ест";
        case DONE:     return "закончил";
    }
    return "?";
}

// Действия философа

static void StartThinking(Table* t, int p) {
    long long duration = RngRange(&t->rng, t->cfg.thinkMin, t->cfg.thinkMax);
    t->phils[p].state = THINKING;
    Say(t, p, "начал размышлять (%lld)", duration);
    Schedule(t, t->now + duration, EV_THINK_END, p, -1);
}

static void StartEating(Table* t, int p) {
    Philosopher* ph = &t->phils[p];
    long long waited = t->now - ph->hungrySince;
    ph->totalWait += waited;
    if (waited > ph->longestWait) {
        ph->longestWait = waited;
    }
    ph->state = EATING;
    long long duration = RngRange(&t->rng, t->cfg.eatMin, t->cfg.eatMax);
    Say(t, p, "начал есть (ждал %lld, будет есть %lld), цикл %d",
        waited, duration, ph->meals + 1);
    Schedule(t, t->now + duration, EV_EAT_END, p, -1);
}

// Философ берёт вилку f. Если теперь у него две вилки - ест, иначе просит вторую.
static void TakeFork(Table* t, int p, int f) {
    t->forks[f].owner = p;
    Say(t, p, "получил вилку %d", f);
    if (ForksHeld(t, p) == 2) {
        StartEating(t, p);
    } else {
        // запрос второй вилки - отдельное событие на то же время,
        // чтобы запросы разных философов шли вперемешку
        Schedule(t, t->now, EV_FORK_REQUEST, p, SecondFork(t, p));
    }
}

// Философ кладёт вилку. Если её ждёт сосед - сразу отдаём ему.
static void ReleaseFork(Table* t, int p, int f) {
    t->forks[f].owner = -1;
    Say(t, p, "положил вилку %d", f);
    int next = t->forks[f].waiter;
    if (next != -1) {
        t->forks[f].waiter = -1;
        TakeFork(t, next, f);
    }
}

// Обработчики событий

static void OnThinkEnd(Table* t, int p) {
    t->phils[p].state = HUNGRY;
    t->phils[p].hungrySince = t->now;
    Say(t, p, "закончил размышлять, проголодался");
    Schedule(t, t->now, EV_FORK_REQUEST, p, FirstFork(t, p));
    // проверяем, поел ли он
    Schedule(t, t->now + t->cfg.maxWait, EV_WAIT_CHECK, p, -1);
}

static void OnForkRequest(Table* t, int p, int f) {
    Say(t, p, "просит вилку %d", f);
    if (t->forks[f].owner == -1) {
        TakeFork(t, p, f);
    } else {
        t->forks[f].waiter = p;
        Say(t, p, "вилка %d занята философом %d, ждёт", f, t->forks[f].owner);
    }
}

static void OnEatEnd(Table* t, int p) {
    Philosopher* ph = &t->phils[p];
    ph->meals++;
    Say(t, p, "закончил есть (поел раз: %d)", ph->meals);
    ReleaseFork(t, p, LeftFork(t, p));
    ReleaseFork(t, p, RightFork(t, p));
    if (t->cfg.cycles > 0 && ph->meals >= t->cfg.cycles) {
        ph->state = DONE;
        t->doneCount++;
        Say(t, p, "поел %d раз и вышел из-за стола", ph->meals);
    } else {
        StartThinking(t, p);
    }
}

// Если философ всё ещё голоден с того же момента - он ждёт слишком долго
static void OnWaitCheck(Table* t, int p) {
    Philosopher* ph = &t->phils[p];
    if (ph->state == HUNGRY && ph->hungrySince + t->cfg.maxWait == t->now) {
        ph->warnings++;
        Say(t, p, "ВНИМАНИЕ: ждёт уже %lld, это предел ожидания", t->cfg.maxWait);
    }
}

// Проверка правил из условия. Возвращает false, если что-то нарушено.
static bool CheckInvariants(Table* t) {
    bool ok = true;
    for (int p = 0; p < t->cfg.n; p++) {
        int held = ForksHeld(t, p);
        int s = t->phils[p].state;
        // ест только с двумя вилками
        if (s == EATING && held != 2) {
            Print("ОШИБКА: философ %d ест, а вилок у него %d\n", p, held);
            ok = false;
        }
        // кто думает или закончил, не держит вилок
        if ((s == THINKING || s == DONE) && held != 0) {
            Print("ОШИБКА: философ %d %s, но держит вилок: %d\n", p, StateName(s), held);
            ok = false;
        }
    }
    for (int f = 0; f < t->cfg.n; f++) {
        // ждать можно только занятую вилку
        if (t->forks[f].waiter != -1 && t->forks[f].owner == -1) {
            Print("ОШИБКА: философ %d ждёт свободную вилку %d\n", t->forks[f].waiter, f);
            ok = false;
        }
    }
    return ok;
}

// Поиск взаимоблокировки: идём по цепочке "ждёт вилку -> её держит другой ->
// он ждёт вилку -> ...". Если вернулись к первому - это замкнутый круг.
static bool FindDeadlock(Table* t) {
    for (int start = 0; start < t->cfg.n; start++) {
        int p = start;
        for (int step = 0; step < t->cfg.n; step++) {
            int f = WaitingFor(t, p);
            if (f == -1) {
                break;
            }
            p = t->forks[f].owner;
            if (p == start) {
                Print("[t=%6lld] ВЗАИМОБЛОКИРОВКА:\n", t->now);
                do {
                    int w = WaitingFor(t, p);
                    Print("    философ %d ждёт вилку %d, её держит философ %d\n",
                          p, w, t->forks[w].owner);
                    p = t->forks[w].owner;
                } while (p != start);
                return true;
            }
        }
    }
    return false;
}

// Главный цикл

int SimRun(Table* t) {
    for (int p = 0; p < t->cfg.n; p++) {
        StartThinking(t, p);
    }
    if (t->cfg.timeLimit > 0) {
        Schedule(t, t->cfg.timeLimit, EV_TIME_LIMIT, -1, -1);
    }

    Event ev;
    while (EventQueuePop(&t->events, &ev)) {
        if (stopSignal != 0) {
            return RESULT_INTERRUPTED;
        }
        t->now = ev.time;
        switch (ev.type) {
            case EV_THINK_END:    OnThinkEnd(t, ev.phil); break;
            case EV_FORK_REQUEST: OnForkRequest(t, ev.phil, ev.fork); break;
            case EV_EAT_END:      OnEatEnd(t, ev.phil); break;
            case EV_WAIT_CHECK:   OnWaitCheck(t, ev.phil); break;
            case EV_TIME_LIMIT:
                Print("[t=%6lld] Время моделирования закончилось\n", t->now);
                return RESULT_TIME_LIMIT;
        }
        // проверяем после того, как событие обработано целиком
        if (!CheckInvariants(t)) {
            return RESULT_ERROR;
        }
        if (FindDeadlock(t)) {
            return RESULT_DEADLOCK;
        }
        if (t->doneCount == t->cfg.n) {
            return RESULT_DONE;
        }
        Pause(t->cfg.delay);
    }
    if (stopSignal != 0) {
        return RESULT_INTERRUPTED;
    }
    // события кончились, а не все поели
    Print("ОШИБКА: события закончились раньше времени\n");
    return RESULT_ERROR;
}

//------------------------------------------------------------------------------
// Вывод результатов

void PrintState(Table* t) {
    Print("Состояние стола (t=%lld):\n", t->now);
    for (int p = 0; p < t->cfg.n; p++) {
        Print("  Философ %d: %s, вилок в руках: %d", p, StateName(t->phils[p].state), ForksHeld(t, p));
        int w = WaitingFor(t, p);
        if (w != -1) {
            Print(", ждёт вилку %d", w);
        }
        Print("\n");
    }
}

void PrintStats(Table* t) {
    Print("\n===== Статистика =====\n");
    int totalMeals = 0, totalWarnings = 0;
    long long totalWait = 0, longest = 0;
    for (int p = 0; p < t->cfg.n; p++) {
        Philosopher* ph = &t->phils[p];
        long long avg = (ph->meals > 0) ? ph->totalWait / ph->meals : 0;
        Print("Философ %d: поел %d раз, ждал всего %lld, в среднем %lld, максимум %lld",
              p, ph->meals, ph->totalWait, avg, ph->longestWait);
        if (ph->warnings > 0) {
            Print(", превышений ожидания: %d", ph->warnings);
        }
        Print("\n");
        totalMeals += ph->meals;
        totalWait += ph->totalWait;
        totalWarnings += ph->warnings;
        if (ph->longestWait > longest) {
            longest = ph->longestWait;
        }
    }
    Print("Всего поели: %d раз\n", totalMeals);
    if (totalMeals > 0) {
        Print("Среднее ожидание: %lld\n", totalWait / totalMeals);
    }
    Print("Самое долгое ожидание: %lld (допустимо %lld)\n", longest, t->cfg.maxWait);
    Print("Превышений допустимого ожидания: %d\n", totalWarnings);
    Print("Модельное время: %lld\n", t->now);
}
