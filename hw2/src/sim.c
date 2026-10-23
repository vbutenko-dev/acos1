#include "sim.h"
#include "rng.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef USE_CONDVAR
#include <semaphore.h>
#endif

volatile sig_atomic_t stopSignal = 0;

enum { THINKING, HUNGRY, EATING, DONE, STOPPED };

// У каждого философа свой семафор или условная переменная.
typedef struct Notification {
#ifdef USE_CONDVAR
    pthread_cond_t cond;
#else
    sem_t *sem;
#ifndef __APPLE__
    sem_t storage;
#endif
#endif
} Notification;

typedef struct Fork {
    int owner;
    int waiter;                // вилку делят только два соседа
} Fork;

struct Table;
typedef struct Philosopher {
    struct Table *table;
    int id;
    int state;
    int waitingFor;
    long long meals;
    long long starts;
    long long warnings;
    bool warned;
    long long hungrySince;
    long long totalWait;
    long long longestWait;
    Rng rng;                   // генератор потока не зависит от порядка планирования
    pthread_t thread;
    Notification changed;
} Philosopher;

typedef struct Table {
    Config cfg;
    Philosopher *phils;
    Fork *forks;
    pthread_mutex_t mutex;     // состояние, владение вилками и целые строки журнала
    bool stopping;            // как и остальные общие данные, защищён mutex
    bool started;
    int firstForkCount;
    int doneCount;
    int result;
    int logFd;
    bool ioFailed;
    long long startMs;
} Table;

static long long ClockMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long Elapsed(const Table *t) {
    return ClockMs() - t->startMs;
}

// Во время работы потоков выводим сообщения под общим мьютексом.
static void Print(Table *t, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int screenResult = vdprintf(STDOUT_FILENO, format, args);
    int screenError = errno;
    va_end(args);
    va_start(args, format);
    int logResult = vdprintf(t->logFd, format, args);
    int logError = errno;
    va_end(args);
    if (screenResult < 0 || logResult < 0) {
        if (!t->ioFailed) {
            dprintf(STDERR_FILENO, "Ошибка записи %s: %s\n",
                    logResult < 0 ? "журнала" : "в терминал",
                    strerror(logResult < 0 ? logError : screenError));
        }
        t->ioFailed = true;
    }
}

static void Say(Table *t, int p, const char *format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Print(t, "[t=%6lld] Философ %d: %s\n", Elapsed(t), p, text);
}

static int NotificationInit(Notification *n, int id) {
#ifdef USE_CONDVAR
    (void)id;
    return pthread_cond_init(&n->cond, NULL);
#else
#ifdef __APPLE__
    char name[32];
    snprintf(name, sizeof(name), "/acos2-%lx-%x", (unsigned long)getpid(), id);
    n->sem = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if (n->sem == SEM_FAILED) {
        return errno;
    }
    if (sem_unlink(name) != 0) {
        int error = errno;
        sem_close(n->sem);
        return error;
    }
    return 0;
#else
    (void)id;
    n->sem = &n->storage;
    return sem_init(n->sem, 0, 0) == 0 ? 0 : errno;
#endif
#endif
}

static void NotificationFree(Notification *n) {
#ifdef USE_CONDVAR
    pthread_cond_destroy(&n->cond);
#else
#ifdef __APPLE__
    sem_close(n->sem);
#else
    sem_destroy(n->sem);
#endif
#endif
}

static int Notify(Philosopher *p) {
#ifdef USE_CONDVAR
    return pthread_cond_signal(&p->changed.cond);
#else
    return sem_post(p->changed.sem) == 0 ? 0 : errno;
#endif
}

static void WakeAllLocked(Table *t) {
    for (int p = 0; p < t->cfg.n; ++p) {
        int error = Notify(&t->phils[p]);
        if (error != 0) {
            Print(t, "ОШИБКА: не удалось разбудить философа %d: %s\n", p, strerror(error));
            t->result = 4;
        }
    }
}

static void StopLocked(Table *t, int result) {
    if (t->stopping) {
        return;
    }
    t->stopping = true;
    t->result = result;
    WakeAllLocked(t);
}

// Вызываем с захваченным мьютексом. Пока ждём, отпускаем его, чтобы сосед
// мог вернуть вилку. После пробуждения снова проверяем условие в цикле.
static bool WaitForChangeLocked(Table *t, Philosopher *p) {
    int error;
#ifdef USE_CONDVAR
    error = pthread_cond_wait(&p->changed.cond, &t->mutex);
#else
    pthread_mutex_unlock(&t->mutex);
    int rc;
    do {
        rc = sem_wait(p->changed.sem);
    } while (rc != 0 && errno == EINTR);
    error = rc == 0 ? 0 : errno;
    pthread_mutex_lock(&t->mutex);
#endif
    if (error != 0) {
        Print(t, "ОШИБКА: ожидание философа %d: %s\n", p->id, strerror(error));
        StopLocked(t, 4);
        return false;
    }
    return !t->stopping;
}

// Для проверки флага там, где мьютекс ещё не захвачен.
static bool IsStopped(Table *t) {
    pthread_mutex_lock(&t->mutex);
    bool stopped = t->stopping;
    pthread_mutex_unlock(&t->mutex);
    return stopped;
}

// Паузы разбиты на короткие отрезки: сигнал или дедлок не задерживают join
// на всю длительность размышления/еды. Вилки во время еды остаются заняты.
static bool Pause(Table *t, long long duration) {
    long long until = ClockMs() + duration;
    while (!IsStopped(t)) {
        long long remaining = until - ClockMs();
        if (remaining <= 0) {
            return true;
        }
        if (remaining > 5) {
            remaining = 5;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = remaining * 1000000L };
        nanosleep(&ts, NULL);
    }
    return false;
}

static int LeftFork(int p) {
    return p;
}

static int RightFork(const Table *t, int p) {
    return (p + 1) % t->cfg.n;
}

static int Held(const Table *t, int p) {
    int count = 0;
    if (t->forks[LeftFork(p)].owner == p) {
        ++count;
    }
    if (t->forks[RightFork(t, p)].owner == p) {
        ++count;
    }
    return count;
}

static bool CheckLocked(Table *t) {
    for (int f = 0; f < t->cfg.n; ++f) {
        int owner = t->forks[f].owner;
        int waiter = t->forks[f].waiter;
        int neighbor = (f + t->cfg.n - 1) % t->cfg.n;
        if (owner != -1 && owner != f && owner != neighbor) {
            Print(t, "ОШИБКА: вилкой %d владеет не её сосед\n", f);
            StopLocked(t, 4);
            return false;
        }
        if (waiter == -1) {
            continue;
        }
        if (owner == -1 || waiter == owner || (waiter != f && waiter != neighbor)) {
            Print(t, "ОШИБКА: неверный ожидающий у вилки %d\n", f);
            StopLocked(t, 4);
            return false;
        }
        if (t->phils[waiter].waitingFor != f) {
            Print(t, "ОШИБКА: философ %d ждёт другую вилку\n", waiter);
            StopLocked(t, 4);
            return false;
        }
    }
    for (int p = 0; p < t->cfg.n; ++p) {
        int held = Held(t, p);
        int state = t->phils[p].state;
        if ((state == EATING && held != 2) ||
            ((state == THINKING || state == DONE || state == STOPPED) && held != 0)) {
            Print(t, "ОШИБКА: состояние философа %d не согласовано с вилками\n", p);
            StopLocked(t, 4);
            return false;
        }
    }
    return true;
}

static void GrantLocked(Table *t, int p, int f) {
    t->forks[f].owner = p;
    Say(t, p, "получил вилку %d", f);
}

static bool TakeFork(Table *t, Philosopher *p, int f) {
    pthread_mutex_lock(&t->mutex);
    if (t->stopping) {
        pthread_mutex_unlock(&t->mutex);
        return false;
    }
    Say(t, p->id, "просит вилку %d", f);
    if (t->forks[f].owner == -1) {
        GrantLocked(t, p->id, f);
    } else {
        // Проверяем вилку и записываем ожидающего под одним мьютексом.
        // Сосед не может вернуть её между этими действиями.
        p->waitingFor = f;
        t->forks[f].waiter = p->id;
        Say(t, p->id, "вилка %d занята философом %d, ждёт", f, t->forks[f].owner);
        while (t->forks[f].owner != p->id && !t->stopping) {
            if (!WaitForChangeLocked(t, p)) {
                break;
            }
        }
    }
    bool ok = !t->stopping && CheckLocked(t);
    pthread_mutex_unlock(&t->mutex);
    return ok;
}

static void PutForkLocked(Table *t, int p, int f) {
    if (t->forks[f].owner != p) {
        return;
    }
    t->forks[f].owner = -1;
    Say(t, p, "положил вилку %d", f);
    int next = t->forks[f].waiter;
    if (next != -1 && !t->stopping) {
        t->forks[f].waiter = -1;
        t->phils[next].waitingFor = -1;
        GrantLocked(t, next, f);
        int error = Notify(&t->phils[next]);
        if (error != 0) {
            Print(t, "ОШИБКА: передача вилки %d: %s\n", f, strerror(error));
            StopLocked(t, 4);
        }
    }
}

static bool FirstForkBarrier(Table *t, Philosopher *p) {
    pthread_mutex_lock(&t->mutex);
    ++t->firstForkCount;
    if (t->firstForkCount == t->cfg.n) {
        WakeAllLocked(t);
    }
    while (t->firstForkCount < t->cfg.n && !t->stopping) {
        if (!WaitForChangeLocked(t, p)) {
            break;
        }
    }
    bool ok = !t->stopping;
    pthread_mutex_unlock(&t->mutex);
    return ok;
}

static void CleanupPhilosopher(Table *t, Philosopher *p) {
    pthread_mutex_lock(&t->mutex);
    if (p->waitingFor != -1) {
        t->forks[p->waitingFor].waiter = -1;
        p->waitingFor = -1;
    }
    if (p->state == EATING) {
        Say(t, p->id, "прервал еду");
    }
    PutForkLocked(t, p->id, LeftFork(p->id));
    PutForkLocked(t, p->id, RightFork(t, p->id));
    p->state = STOPPED;
    Say(t, p->id, "остановлен");
    pthread_mutex_unlock(&t->mutex);
}

static void *PhilosopherRun(void *arg) {
    Philosopher *p = arg;
    Table *t = p->table;
    pthread_mutex_lock(&t->mutex);
    while (!t->started && !t->stopping) {
        if (!WaitForChangeLocked(t, p)) {
            break;
        }
    }
    pthread_mutex_unlock(&t->mutex);

    while (!IsStopped(t)) {
        long long think = RngRange(&p->rng, t->cfg.thinkMin, t->cfg.thinkMax);
        pthread_mutex_lock(&t->mutex);
        if (t->stopping) {
            pthread_mutex_unlock(&t->mutex);
            break;
        }
        p->state = THINKING;
        Say(t, p->id, "начал размышлять (%lld мс)", think);
        pthread_mutex_unlock(&t->mutex);
        if (!Pause(t, think) || !Pause(t, t->cfg.delay)) {
            break;
        }

        pthread_mutex_lock(&t->mutex);
        if (t->stopping) {
            pthread_mutex_unlock(&t->mutex);
            break;
        }
        p->state = HUNGRY;
        p->hungrySince = Elapsed(t);
        p->warned = false;
        Say(t, p->id, "закончил размышлять, проголодался");
        pthread_mutex_unlock(&t->mutex);

        int first = LeftFork(p->id), second = RightFork(t, p->id);
        if (t->cfg.strategy == STRATEGY_ORDERED && first > second) {
            int tmp = first;
            first = second;
            second = tmp;
        }
        if (!TakeFork(t, p, first)) {
            break;
        }
        if (t->cfg.syncFirstFork && p->meals == 0 && !FirstForkBarrier(t, p)) {
            break;
        }
        if (!TakeFork(t, p, second)) {
            break;
        }

        long long eat = RngRange(&p->rng, t->cfg.eatMin, t->cfg.eatMax);
        pthread_mutex_lock(&t->mutex);
        if (t->stopping) {
            pthread_mutex_unlock(&t->mutex);
            break;
        }
        long long waited = Elapsed(t) - p->hungrySince;
        p->totalWait += waited;
        if (waited > p->longestWait) {
            p->longestWait = waited;
        }
        ++p->starts;
        p->state = EATING;
        Say(t, p->id, "начал есть (ждал %lld мс, будет есть %lld мс), цикл %lld",
            waited, eat, p->meals + 1);
        bool valid = CheckLocked(t);
        pthread_mutex_unlock(&t->mutex);
        if (!valid || !Pause(t, eat)) {
            break;
        }

        pthread_mutex_lock(&t->mutex);
        if (t->stopping) {
            pthread_mutex_unlock(&t->mutex);
            break;
        }
        ++p->meals;
        Say(t, p->id, "закончил есть (поел раз: %lld)", p->meals);
        PutForkLocked(t, p->id, LeftFork(p->id));
        PutForkLocked(t, p->id, RightFork(t, p->id));
        bool done = t->cfg.cycles > 0 && p->meals >= t->cfg.cycles;
        p->state = done ? DONE : THINKING;
        if (done) {
            ++t->doneCount;
            Say(t, p->id, "поел %lld раз и вышел из-за стола", p->meals);
        }
        valid = CheckLocked(t);
        pthread_mutex_unlock(&t->mutex);
        if (done) {
            return NULL;
        }
        if (!valid || !Pause(t, t->cfg.delay)) {
            break;
        }
    }
    CleanupPhilosopher(t, p);
    return NULL;
}

static bool FindDeadlockLocked(Table *t) {
    for (int start = 0; start < t->cfg.n; ++start) {
        int p = start;
        for (int step = 0; step < t->cfg.n; ++step) {
            int f = t->phils[p].waitingFor;
            if (f == -1 || t->forks[f].owner == -1) {
                break;
            }
            p = t->forks[f].owner;
            if (p == start) {
                Print(t, "[t=%6lld] ВЗАИМОБЛОКИРОВКА:\n", Elapsed(t));
                do {
                    int waiting = t->phils[p].waitingFor;
                    int owner = t->forks[waiting].owner;
                    Print(t, "  философ %d ждёт вилку %d, её держит философ %d\n", p, waiting, owner);
                    p = owner;
                } while (p != start);
                return true;
            }
        }
    }
    return false;
}

static void Monitor(Table *t) {
    while (!IsStopped(t)) {
        pthread_mutex_lock(&t->mutex);
        long long now = Elapsed(t);
        if (t->ioFailed) {
            StopLocked(t, 1);
        } else if (stopSignal != 0) {
            Print(t, "Моделирование прервано (сигнал %d).\n", (int)stopSignal);
            StopLocked(t, 128 + stopSignal);
        } else if (!CheckLocked(t)) {
            // CheckLocked уже записал причину и разбудил ожидающих.
        } else if (t->doneCount == t->cfg.n) {
            Print(t, "Все философы поели нужное число раз.\n");
            StopLocked(t, 0);
        } else if (FindDeadlockLocked(t)) {
            StopLocked(t, 3);
        } else if (t->cfg.timeLimit > 0 && now >= t->cfg.timeLimit) {
            if (t->cfg.cycles == 0) {
                Print(t, "Моделирование закончено по времени.\n");
            } else {
                Print(t, "Время вышло: все циклы завершили %d философов из %d.\n", t->doneCount, t->cfg.n);
            }
            StopLocked(t, t->cfg.cycles == 0 ? 0 : 5);
        } else {
            for (int p = 0; p < t->cfg.n; ++p) {
                Philosopher *ph = &t->phils[p];
                long long waited = now - ph->hungrySince;
                if (ph->state == HUNGRY && !ph->warned && waited >= t->cfg.maxWait) {
                    ph->warned = true;
                    ++ph->warnings;
                    Say(t, p, "ВНИМАНИЕ: ждёт %lld мс, предел %lld мс", waited, t->cfg.maxWait);
                }
            }
        }
        pthread_mutex_unlock(&t->mutex);
        if (!IsStopped(t)) {
            struct timespec interval = { .tv_sec = 0, .tv_nsec = 1000000L };
            nanosleep(&interval, NULL);
        }
    }
}

static void PrintStats(Table *t) {
    long long totalMeals = 0, totalWait = 0, totalStarts = 0, longest = 0;
    int occupied = 0;
    Print(t, "\n===== Статистика =====\n");
    for (int p = 0; p < t->cfg.n; ++p) {
        Philosopher *ph = &t->phils[p];
        long long average = ph->starts == 0 ? 0 : ph->totalWait / ph->starts;
        Print(t, "Философ %d: поел %lld раз, начал есть %lld раз, ждал всего %lld мс, в среднем %lld мс, максимум %lld мс, предупреждений %lld\n",
              p, ph->meals, ph->starts, ph->totalWait, average, ph->longestWait, ph->warnings);
        totalMeals += ph->meals;
        totalWait += ph->totalWait;
        totalStarts += ph->starts;
        if (ph->longestWait > longest) {
            longest = ph->longestWait;
        }
        if (t->forks[p].owner != -1) {
            ++occupied;
        }
    }
    Print(t, "Всего поели: %lld раз\n", totalMeals);
    Print(t, "Среднее ожидание: %lld мс\n", totalStarts == 0 ? 0 : totalWait / totalStarts);
    Print(t, "Самое долгое ожидание: %lld мс\n", longest);
    Print(t, "Вилок занято после завершения: %d\n", occupied);
    Print(t, "Прошло времени: %lld мс\n", Elapsed(t));
}

int RunSimulation(const Config *cfg, int logFd) {
    Table t = { .cfg = *cfg, .logFd = logFd, .startMs = ClockMs() };
    t.phils = calloc((size_t)cfg->n, sizeof(*t.phils));
    t.forks = calloc((size_t)cfg->n, sizeof(*t.forks));
    if (t.phils == NULL || t.forks == NULL) {
        dprintf(STDERR_FILENO, "Не хватило памяти\n");
        free(t.phils);
        free(t.forks);
        return 1;
    }
    int error = pthread_mutex_init(&t.mutex, NULL);
    if (error != 0) {
        dprintf(STDERR_FILENO, "Не удалось создать мьютекс: %s\n", strerror(error));
        free(t.phils);
        free(t.forks);
        return 1;
    }
    int initialized = 0;
    for (int p = 0; p < cfg->n; ++p) {
        Philosopher *ph = &t.phils[p];
        ph->table = &t;
        ph->id = p;
        ph->state = THINKING;
        ph->waitingFor = -1;
        RngSeed(&ph->rng, cfg->seed ^ (UINT64_C(0xD1B54A32D192ED03) * (uint64_t)(p + 1)));
        t.forks[p].owner = -1;
        t.forks[p].waiter = -1;
        error = NotificationInit(&ph->changed, p);
        if (error != 0) {
            break;
        }
        ++initialized;
    }
    if (error != 0) {
        Print(&t, "Не удалось создать синхропримитив: %s\n", strerror(error));
        t.result = 1;
    } else {
#ifdef USE_CONDVAR
        const char *backend = "mutex + condition variables";
#else
        const char *backend = "mutex + semaphores";
#endif
        Print(&t, "Обедающие философы, ДЗ2: потоков-философов %d, циклов %d, стратегия %s, seed %llu\n",
              cfg->n, cfg->cycles, cfg->strategy == STRATEGY_NAIVE ? "naive" : "ordered", (unsigned long long)cfg->seed);
        Print(&t, "Синхронизация: %s. Время — реальные миллисекунды.\n", backend);

        // Рабочие потоки наследуют заблокированные SIGINT/SIGTERM. После
        // создания маска main восстанавливается, обработчик работает в main.
        sigset_t blocked, previous;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGINT);
        sigaddset(&blocked, SIGTERM);
        error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
        int created = 0;
        if (error != 0) {
            Print(&t, "Не удалось установить маску сигналов: %s\n", strerror(error));
            t.result = 1;
        } else {
            for (int p = 0; p < cfg->n; ++p) {
                error = pthread_create(&t.phils[p].thread, NULL, PhilosopherRun, &t.phils[p]);
                if (error != 0) {
                    break;
                }
                ++created;
            }
            pthread_mutex_lock(&t.mutex);
            if (error != 0) {
                Print(&t, "Не удалось создать поток %d: %s\n", created, strerror(error));
                StopLocked(&t, 1);
            } else {
                t.startMs = ClockMs();
                t.started = true;
                WakeAllLocked(&t);
            }
            pthread_mutex_unlock(&t.mutex);
            int maskError = pthread_sigmask(SIG_SETMASK, &previous, NULL);
            if (maskError != 0) {
                pthread_mutex_lock(&t.mutex);
                Print(&t, "Не удалось восстановить маску сигналов: %s\n", strerror(maskError));
                StopLocked(&t, 1);
                pthread_mutex_unlock(&t.mutex);
            }
            if (!IsStopped(&t)) {
                Monitor(&t);
            }
            // Освобождаем общие данные только после завершения всех потоков.
            for (int p = 0; p < created; ++p) {
                int joinError = pthread_join(t.phils[p].thread, NULL);
                if (joinError != 0) {
                    // Если завершение потока не подтверждено, освобождать
                    // его общие данные нельзя. Останавливаем весь процесс.
                    dprintf(STDERR_FILENO, "ОШИБКА: pthread_join: %s\n", strerror(joinError));
                    _Exit(4);
                }
            }
            PrintStats(&t);
            if (!CheckLocked(&t)) {
                t.result = 4;
            }
        }
    }
    if (t.ioFailed && t.result == 0) {
        t.result = 1;
    }
    Print(&t, "Код завершения: %d\n", t.result);
    if (t.ioFailed && t.result == 0) {
        t.result = 1;
    }
    for (int p = 0; p < initialized; ++p) {
        NotificationFree(&t.phils[p].changed);
    }
    pthread_mutex_destroy(&t.mutex);
    free(t.phils);
    free(t.forks);
    return t.result;
}
