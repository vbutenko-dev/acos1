#include "sim.h"
#include "rng.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { THINKING, HUNGRY, GRANTED, DONE };

// В shared memory нет указателей: процессы могут отобразить её по разным адресам.
typedef struct Shared {
    Config cfg;
    sem_t mutex, ready[MAX_PHILOSOPHERS];  // используются в unnamed-версии
    int owner[MAX_PHILOSOPHERS];
    int state[MAX_PHILOSOPHERS], meals[MAX_PHILOSOPHERS];
    pid_t pid[MAX_PHILOSOPHERS];
    int queue[MAX_PHILOSOPHERS], head, count;
    int joined, finished, started, stop, result;
    long long startMs, hungrySince[MAX_PHILOSOPHERS];
    long long wait[MAX_PHILOSOPHERS], maxWait[MAX_PHILOSOPHERS];
    char logPath[PATH_MAX];
} Shared;

// Указатели и файловые дескрипторы принадлежат конкретному процессу.
typedef struct Table {
    Shared *s;
    sem_t *mutex, *ready[MAX_PHILOSOPHERS];
    char name[64];
    int named, readyCount, mutexCreated, logFd;
} Table;

static volatile sig_atomic_t stopSignal;

static void OnSignal(int number) { stopSignal = number; }

int InstallSignals(void) {
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = OnSignal;
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return -1;
    action.sa_handler = SIG_IGN;  // ошибка write обрабатывается обычным кодом
    return sigaction(SIGPIPE, &action, NULL);
}

static long long ClockMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void Tick(void) {
    struct timespec ts = {0, 5000000};
    nanosleep(&ts, NULL);
}

static int ValidKey(const char *key) {
    size_t length = strlen(key);
    if (length == 0 || length > 20) return 0;
    return strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == length;
}

static void Stop(Table *t, int result) {
    if (t->s->stop) return;
    t->s->stop = 1;
    t->s->result = result;
    for (int i = 0; i < t->s->cfg.n; ++i) sem_post(t->ready[i]);
}

static void Lock(Table *t) {
    while (sem_wait(t->mutex) != 0) {
        if (errno != EINTR) { perror("sem_wait mutex"); _exit(1); }
    }
    if (stopSignal) Stop(t, 128 + stopSignal);
}

static void Unlock(Table *t) { sem_post(t->mutex); }

// Все сообщения пишутся под общим семафором: строки процессов не перемешиваются.
static void Print(Table *t, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int screen = vdprintf(STDOUT_FILENO, format, args);
    va_end(args);
    va_start(args, format);
    int log = vdprintf(t->logFd, format, args);
    va_end(args);
    if (screen < 0 || log < 0) {
        dprintf(STDERR_FILENO, "Ошибка записи вывода или журнала\n");
        Stop(t, 1);
    }
}

static void Say(Table *t, int id, const char *format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Print(t, "[t=%6lld] Философ %d: %s\n", ClockMs() - t->s->startMs, id, message);
}

static void SemName(const Table *t, int id, char *buffer, size_t size) {
    if (id < 0) snprintf(buffer, size, "%s-lock", t->name);
    else snprintf(buffer, size, "%s-p%d", t->name, id);
}

static void CloseTable(Table *t, int owner) {
    char name[80];
    for (int i = 0; i < t->readyCount; ++i) {
        if (t->named) {
            sem_close(t->ready[i]);
            if (owner) { SemName(t, i, name, sizeof(name)); sem_unlink(name); }
        } else if (owner) sem_destroy(t->ready[i]);
    }
    if (t->mutexCreated) {
        if (t->named) {
            sem_close(t->mutex);
            if (owner) { SemName(t, -1, name, sizeof(name)); sem_unlink(name); }
        } else if (owner) sem_destroy(t->mutex);
    }
    if (t->s) munmap(t->s, sizeof(Shared));
    if (owner) shm_unlink(t->name);
    if (t->logFd >= 0) close(t->logFd);
}

static int OpenTable(Table *t, const Config *cfg) {
    int owner = cfg != NULL;
    int fd = shm_open(t->name, O_RDWR | (owner ? O_CREAT | O_EXCL : 0), 0600);
    if (fd < 0) { perror("shm_open (проверьте ключ и запущенный стол)"); return -1; }
    if (owner && ftruncate(fd, sizeof(Shared)) != 0) {
        close(fd); shm_unlink(t->name); return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size != sizeof(Shared)) {
        close(fd);
        if (owner) shm_unlink(t->name);
        errno = EINVAL;
        return -1;
    }
    t->s = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (t->s == MAP_FAILED) {
        t->s = NULL;
        if (owner) shm_unlink(t->name);
        return -1;
    }
    if (owner) {
        memset(t->s, 0, sizeof(Shared));
        t->s->cfg = *cfg;
        for (int i = 0; i < cfg->n; ++i) t->s->owner[i] = -1;
    }
    char name[80];
    // Создаём mutex последним, когда остальные общие данные уже готовы.
    for (int i = 0; i < t->s->cfg.n; ++i) {
        if (t->named) {
            SemName(t, i, name, sizeof(name));
            t->ready[i] = sem_open(name, owner ? O_CREAT | O_EXCL : 0, 0600, 0);
            if (t->ready[i] == SEM_FAILED) goto fail;
        } else {
            t->ready[i] = &t->s->ready[i];
            if (sem_init(t->ready[i], 1, 0) != 0) goto fail;
        }
        ++t->readyCount;
    }
    if (t->named) {
        SemName(t, -1, name, sizeof(name));
        t->mutex = sem_open(name, owner ? O_CREAT | O_EXCL : 0, 0600, 1);
        if (t->mutex == SEM_FAILED) goto fail;
    } else {
        t->mutex = &t->s->mutex;
        if (sem_init(t->mutex, 1, 1) != 0) goto fail;
    }
    t->mutexCreated = 1;
    return 0;
fail:
    perror("инициализация семафоров");
    CloseTable(t, owner);
    return -1;
}

// Строгая FIFO: новые голодные не обгоняют голову очереди. Обе вилки
// резервируются под mutex; удержания одной вилки при ожидании здесь нет.
static void Schedule(Table *t) {
    Shared *s = t->s;
    while (s->count && !s->stop) {
        int id = s->queue[s->head], right = (id + 1) % s->cfg.n;
        long long waiting = ClockMs() - s->hungrySince[id];
        if (waiting >= s->cfg.maxWait) {
            Print(t, "Философ %d превысил допустимое ожидание %lld мс.\n", id, s->cfg.maxWait);
            Stop(t, 5);
            break;
        }
        if (s->owner[id] != -1 || s->owner[right] != -1) break;
        s->head = (s->head + 1) % s->cfg.n;
        --s->count;
        s->owner[id] = s->owner[right] = id;
        s->state[id] = GRANTED;
        s->wait[id] = waiting;
        if (s->wait[id] > s->maxWait[id]) s->maxWait[id] = s->wait[id];
        Say(t, id, "получил вилку %d", id);
        Say(t, id, "получил вилку %d", right);
        sem_post(t->ready[id]);
    }
}

static int Pause(Table *t, long long duration) {
    long long until = ClockMs() + duration;
    for (;;) {
        Lock(t);
        int stopped = t->s->stop;
        Unlock(t);
        if (stopped) return 0;
        if (ClockMs() >= until) return 1;
        Tick();
    }
}

static int WaitReady(sem_t *ready) {
    // Короткий таймаут закрывает окно, когда сигнал пришёл прямо перед wait.
    while (!stopSignal) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 20000000;
        if (until.tv_nsec >= 1000000000) {
            ++until.tv_sec;
            until.tv_nsec -= 1000000000;
        }
        if (sem_timedwait(ready, &until) == 0) return 0;
        if (errno != EINTR && errno != ETIMEDOUT) return -1;
    }
    return -1;
}

static int Philosopher(Table *t, int id) {
    Shared *s = t->s;
    Lock(t);
    if (id >= s->cfg.n || s->pid[id] || s->stop) {
        Unlock(t);
        dprintf(2, "Философ %d: номер занят, вне диапазона или стол остановлен\n", id);
        return 1;
    }
    s->pid[id] = getpid();
    ++s->joined;
    Unlock(t);
    for (;;) {
        Lock(t);
        int ready = s->started || s->stop;
        Unlock(t);
        if (ready) break;
        Tick();
    }
    Rng rng;
    RngSeed(&rng, s->cfg.seed + (uint64_t)id);
    while (!s->cfg.cycles || s->meals[id] < s->cfg.cycles) {
        long long think = RngRange(&rng, s->cfg.thinkMin, s->cfg.thinkMax);
        Lock(t);
        if (s->stop) { Unlock(t); break; }
        Say(t, id, "начал размышлять (%lld мс)", think);
        Unlock(t);
        if (!Pause(t, think)) break;
        Lock(t);
        if (s->stop) { Unlock(t); break; }
        Say(t, id, "закончил размышлять");
        Say(t, id, "проголодался");
        Say(t, id, "просит вилку %d", id);
        Say(t, id, "просит вилку %d", (id + 1) % s->cfg.n);
        s->state[id] = HUNGRY;
        s->hungrySince[id] = ClockMs();
        s->queue[(s->head + s->count++) % s->cfg.n] = id;
        Schedule(t);
        Unlock(t);
        int rc = WaitReady(t->ready[id]);
        Lock(t);
        if (rc != 0 && !stopSignal) Stop(t, 1);
        if (s->stop) { Unlock(t); break; }
        long long eat = RngRange(&rng, s->cfg.eatMin, s->cfg.eatMax);
        Say(t, id, "начал есть (ждал %lld мс, будет есть %lld мс), цикл %d",
            s->wait[id], eat, s->meals[id] + 1);
        Unlock(t);
        Pause(t, eat);
        Lock(t);
        if (!s->stop) Say(t, id, "закончил есть (поел раз: %d)", ++s->meals[id]);
        else Say(t, id, "прервал еду");
        for (int f = 0; f < 2; ++f) {
            int fork = (id + f) % s->cfg.n;
            s->owner[fork] = -1;
            Say(t, id, "положил вилку %d", fork);
        }
        s->state[id] = THINKING;
        Schedule(t);
        Unlock(t);
    }
    Lock(t);
    if (s->state[id] == HUNGRY) {
        long long waited = ClockMs() - s->hungrySince[id];
        if (waited > s->maxWait[id]) s->maxWait[id] = waited;
        Say(t, id, "прервал ожидание (ждал %lld мс)", waited);
    }
    // Остановка могла прийти после выдачи вилок, но до начала еды.
    for (int f = 0; f < s->cfg.n; ++f) {
        if (s->owner[f] == id) {
            s->owner[f] = -1;
            Say(t, id, "положил вилку %d", f);
        }
    }
    s->state[id] = DONE;
    ++s->finished;
    int result = s->result;
    Unlock(t);
    return result;
}

int RunPhilosopher(const char *key, int id) {
    if (!ValidKey(key)) { dprintf(2, "Неверный ключ стола\n"); return 2; }
    Table t = {.named = 1, .logFd = -1};
    snprintf(t.name, sizeof(t.name), "/acos3-%s", key);
    if (OpenTable(&t, NULL) != 0) return 1;
    t.logFd = open(t.s->logPath, O_WRONLY | O_APPEND);
    if (t.logFd < 0) { perror("open log"); CloseTable(&t, 0); return 1; }
    int result = Philosopher(&t, id);
    CloseTable(&t, 0);
    return result;
}

int RunTable(const Config *cfg, const char *key, const char *logPath,
             int independent, int named) {
    if (!ValidKey(key)) { dprintf(2, "Неверный ключ стола\n"); return 2; }
    Table t = {.named = named, .logFd = -1};
    snprintf(t.name, sizeof(t.name), "/acos3-%s", key);
    if (OpenTable(&t, cfg) != 0) return 1;
    t.logFd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (t.logFd < 0 || !realpath(logPath, t.s->logPath)) {
        perror("open log"); CloseTable(&t, 1); return 1;
    }
    Lock(&t);
    Print(&t, "Стол готов: %s\nФилософов и вилок: %d, стратегия: fifo, семафоры: %s\n",
          key, cfg->n, named ? "именованные" : "неименованные");
    Unlock(&t);
    pid_t children[MAX_PHILOSOPHERS] = {0};
    if (!independent) {
        for (int i = 0; i < cfg->n; ++i) {
            pid_t pid = fork();
            if (pid == 0) {
                int result = Philosopher(&t, i);
                CloseTable(&t, 0);
                _exit(result);
            }
            if (pid < 0) { Lock(&t); Stop(&t, 1); Unlock(&t); break; }
            children[i] = pid;
        }
    }
    long long created = ClockMs();
    for (;;) {
        Lock(&t);
        Shared *s = t.s;
        if (!s->started && s->joined == cfg->n && !s->stop) {
            s->startMs = ClockMs();
            s->started = 1;
            Print(&t, "Все философы подключены, начало моделирования.\n");
        }
        if (!s->started && !s->stop && ClockMs() - created >= 30000) {
            Print(&t, "Не все философы подключились за 30 секунд.\n");
            Stop(&t, 5);
        }
        if (s->started && !s->stop && cfg->timeLimit &&
            ClockMs() - s->startMs >= cfg->timeLimit && s->finished != cfg->n) {
            Print(&t, "Достигнут предел времени %lld мс.\n", cfg->timeLimit);
            Stop(&t, cfg->cycles ? 5 : 0);
        }
        for (int i = 0; i < cfg->n && !s->stop; ++i) {
            if (s->state[i] == HUNGRY && ClockMs() - s->hungrySince[i] >= cfg->maxWait) {
                Print(&t, "Философ %d превысил допустимое ожидание %lld мс.\n", i, cfg->maxWait);
                Stop(&t, 5);
            }
        }
        int finished = s->stop ? s->finished == s->joined : s->finished == cfg->n;
        Unlock(&t);
        if (!independent) {
            int alive = 0;
            for (int i = 0; i < cfg->n; ++i) {
                if (!children[i]) continue;
                int status;
                pid_t ended = waitpid(children[i], &status, WNOHANG);
                if (ended == children[i]) {
                    children[i] = 0;
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                        Lock(&t); Stop(&t, 1); Unlock(&t);
                    }
                } else ++alive;
            }
            finished = alive == 0;
        }
        if (finished) break;
        Tick();
    }
    Lock(&t);
    long long total = 0;
    int occupied = 0;
    for (int i = 0; i < cfg->n; ++i) {
        total += t.s->meals[i];
        occupied += t.s->owner[i] != -1;
        Print(&t, "Философ %d: поел %d раз, максимальное ожидание %lld мс\n",
              i, t.s->meals[i], t.s->maxWait[i]);
    }
    if (t.s->result == 0 && !t.s->stop) Print(&t, "Все философы поели нужное число раз.\n");
    Print(&t, "Всего поели: %lld раз\nВилок занято после завершения: %d\nКод завершения: %d\n",
          total, occupied, t.s->result);
    int result = t.s->result;
    Unlock(&t);
    CloseTable(&t, 1);
    return result;
}
