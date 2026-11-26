#include "net.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { MAX_CONNECTIONS = MAX_PHILOSOPHERS + 32, REGISTER_MS = 30000 };
enum { UNREGISTERED = -1, OBSERVER = -2 };

typedef enum { OFFLINE, READY, THINKING, WAITING, EATING, DONE } State;

typedef struct {
    int peer;
    State state;
    long long meals;
    long long hungryAt;
} Philosopher;

typedef struct {
    int n, cycles, maxWait, timeLimit;
    Philosopher philosophers[MAX_PHILOSOPHERS];
    int queue[MAX_PHILOSOPHERS], waiting;
    int registered, finished;
    long long openedAt, startedAt;
    struct pollfd sockets[MAX_CONNECTIONS + 1];  // 0 — слушающий сокет
    int roles[MAX_CONNECTIONS + 1];
} Table;

static volatile sig_atomic_t stopped;

static void Stop(int signalNumber) {
    stopped = signalNumber;
}

static void ClosePeer(Table *table, int peer) {
    close(table->sockets[peer].fd);
    table->sockets[peer].fd = -1;
}

static int SendLine(Table *table, int peer, const char *line) {
    size_t length = strlen(line);
    if (send(table->sockets[peer].fd, line, length, 0) == (ssize_t)length) return 1;
    // Если наблюдатель перестал читать, стол продолжает работать.
    ClosePeer(table, peer);
    return 0;
}

static void Event(Table *table, const char *format, ...) {
    char message[256], line[320];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    long long time = table->startedAt ? NowMs() - table->startedAt : 0;
    snprintf(line, sizeof(line), "[%6lld мс] %s\n", time, message);
    dprintf(1, "%s", line);
    for (int i = 1; i <= MAX_CONNECTIONS; ++i) {
        if (table->sockets[i].fd >= 0 && table->roles[i] == OBSERVER) {
            SendLine(table, i, line);
        }
    }
}

static void Snapshot(Table *table, int peer) {
    static const char *states[] = {
        "не подключён", "подключён, ждёт старта", "размышляет", "ждёт вилки",
        "ест", "закончил",
    };
    char line[256];
    snprintf(line, sizeof(line), "Состояние стола: подключилось %d из %d, %s\n",
             table->registered, table->n,
             table->startedAt ? "моделирование идёт" : "ожидаем участников");
    if (!SendLine(table, peer, line)) return;
    for (int id = 0; id < table->n; ++id) {
        Philosopher *philosopher = &table->philosophers[id];
        snprintf(line, sizeof(line), "Философ %d: %s, поел %lld раз\n",
                 id, states[philosopher->state], philosopher->meals);
        if (!SendLine(table, peer, line)) return;
    }
}

static int Tell(Table *table, int id, unsigned char command) {
    if (SendByte(table->sockets[table->philosophers[id].peer].fd, command)) return 1;
    Event(table, "Философ %d: не удалось отправить команду", id);
    return 0;
}

static int StartThinking(Table *table, int id) {
    table->philosophers[id].state = THINKING;
    Event(table, "Философ %d: начал размышлять", id);
    return Tell(table, id, THINK);
}

static int Start(Table *table) {
    table->startedAt = NowMs();
    Event(table, "Все философы подключились. Начало моделирования");
    for (int i = 0; i < table->n; ++i) {
        if (!StartThinking(table, i)) return 0;
    }
    return 1;
}

static int GiveForks(Table *table) {
    while (table->waiting > 0) {
        int id = table->queue[0];
        int left = (id + table->n - 1) % table->n;
        int right = (id + 1) % table->n;

        // Едящий сосед держит общую вилку. Выдаём только свободную пару.
        if (table->philosophers[left].state == EATING ||
            table->philosophers[right].state == EATING) break;

        --table->waiting;
        memmove(table->queue, table->queue + 1, table->waiting * sizeof(table->queue[0]));

        Philosopher *philosopher = &table->philosophers[id];
        philosopher->state = EATING;
        Event(table, "Философ %d: получил вилки %d и %d", id, id, right);
        Event(table, "Философ %d: начал есть, ждал %lld мс",
              id, NowMs() - philosopher->hungryAt);
        if (!Tell(table, id, EAT)) return 0;
    }
    return 1;
}

static int Play(Table *table, int id, unsigned char command) {
    Philosopher *philosopher = &table->philosophers[id];
    int right = (id + 1) % table->n;
    if (command == HUNGRY && philosopher->state == THINKING) {
        philosopher->state = WAITING;
        philosopher->hungryAt = NowMs();
        table->queue[table->waiting++] = id;
        Event(table, "Философ %d: закончил размышлять, проголодался", id);
        Event(table, "Философ %d: просит вилки %d и %d", id, id, right);
    } else if (command == RELEASE && philosopher->state == EATING) {
        ++philosopher->meals;
        Event(table, "Философ %d: закончил есть, поел %lld раз", id, philosopher->meals);
        Event(table, "Философ %d: освободил вилки %d и %d", id, id, right);
        if (table->cycles > 0 && philosopher->meals == table->cycles) {
            philosopher->state = DONE;
            ++table->finished;
            Event(table, "Философ %d: выполнил все циклы", id);
            if (!Tell(table, id, FINISH)) return 0;
            ClosePeer(table, philosopher->peer);
        } else if (!StartThinking(table, id)) {
            return 0;
        }
    } else {
        Event(table, "Философ %d: команда не подходит текущему состоянию", id);
        return 0;
    }
    return GiveForks(table);
}

static int ReadPeer(Table *table, int peer) {
    int role = table->roles[peer];
    unsigned char byte;
    ssize_t result = recv(table->sockets[peer].fd, &byte, 1, 0);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 1;
    if (result != 1 || role == OBSERVER) {
        ClosePeer(table, peer);
        if (role < 0) return 1;
        Event(table, "Философ %d: отключился до завершения", role);
        return 0;
    }
    if (role >= 0) return Play(table, role, byte);

    if (byte == 0) {
        table->roles[peer] = OBSERVER;
        Snapshot(table, peer);
        return 1;
    }
    int id = byte - 1;
    if (id >= table->n || table->philosophers[id].state != OFFLINE) {
        ClosePeer(table, peer);
        return 1;
    }
    table->roles[peer] = id;
    table->philosophers[id].peer = peer;
    table->philosophers[id].state = READY;
    ++table->registered;
    Event(table, "Философ %d: подключился (%d/%d)", id, table->registered, table->n);
    return table->registered != table->n || Start(table);
}

static void AcceptPeer(Table *table) {
    int fd = accept(table->sockets[0].fd, NULL, NULL);
    if (fd < 0) return;
    for (int i = 1; i <= MAX_CONNECTIONS; ++i) {
        if (table->sockets[i].fd >= 0) continue;
        if (SetNonblocking(fd) < 0) break;
        table->sockets[i] = (struct pollfd){fd, POLLIN, 0};
        table->roles[i] = UNREGISTERED;
        return;
    }
    close(fd);
}

static int Limit(Table *table) {
    long long now = NowMs();
    if (!table->startedAt) {
        if (now - table->openedAt < REGISTER_MS) return -1;
        Event(table, "Не все философы подключились за %d мс", REGISTER_MS);
        return 5;
    }
    if (table->timeLimit && now - table->startedAt >= table->timeLimit) {
        Event(table, "Истёк предел времени моделирования (%d мс)", table->timeLimit);
        return table->cycles == 0 ? 0 : 5;
    }
    if (table->waiting > 0) {
        int id = table->queue[0];  // этот философ ждёт дольше остальных
        long long wait = now - table->philosophers[id].hungryAt;
        if (wait >= table->maxWait) {
            Event(table, "Философ %d: превысил предел ожидания вилок (%lld мс)", id, wait);
            return 5;
        }
    }
    return -1;
}

static int Run(Table *table) {
    table->openedAt = NowMs();
    while (!stopped && table->finished < table->n) {
        if (poll(table->sockets, MAX_CONNECTIONS + 1, 25) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }
        if (stopped) break;
        int result = Limit(table);
        if (result >= 0) return result;
        for (int i = 1; i <= MAX_CONNECTIONS; ++i) {
            if (table->sockets[i].fd >= 0 && table->sockets[i].revents && !ReadPeer(table, i)) {
                return 1;
            }
        }
        if (table->sockets[0].revents & POLLIN) AcceptPeer(table);
    }
    return stopped ? 128 + stopped : 0;
}

static void Help(const char *program) {
    dprintf(1, "Использование: %s IP PORT [-n N] [-c N] [-s fifo] [-w MS] [-T MS]\n"
            "  -n — философов и вилок, 2..64 (5)\n"
            "  -c — циклов у каждого, 0 — без ограничения (3)\n"
            "  -s fifo — выдача двух вилок по очереди FIFO\n"
            "  -w — предел ожидания вилок, больше нуля (2000 мс)\n"
            "  -T — предел времени, 0 — без ограничения (0)\n"
            "  IP 0.0.0.0 — слушать все интерфейсы.\n",
            program);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        Help(argv[0]);
        return 0;
    }
    if (argc < 3) {
        Help(argv[0]);
        return 2;
    }
    Table table = {.n = 5, .cycles = 3, .maxWait = 2000};
    int option, valid = 1;
    optind = 3;
    while ((option = getopt(argc, argv, "n:c:s:w:T:")) != -1) {
        uint64_t value = 0;
        switch (option) {
            case 'n':
                valid = Number(optarg, MAX_PHILOSOPHERS, &value) && value >= 2;
                table.n = (int)value;
                break;
            case 'c':
                valid = Number(optarg, INT_MAX, &value);
                table.cycles = (int)value;
                break;
            case 's':
                valid = strcmp(optarg, "fifo") == 0;
                break;
            case 'w':
                valid = Number(optarg, MAX_TIME_MS, &value) && value > 0;
                table.maxWait = (int)value;
                break;
            case 'T':
                valid = Number(optarg, MAX_TIME_MS, &value);
                table.timeLimit = (int)value;
                break;
            default:
                valid = 0;
                break;
        }
        if (!valid) break;
    }
    if (!valid || optind != argc) {
        Help(argv[0]);
        return 2;
    }

    struct sigaction action = {.sa_handler = Stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    int listener = OpenListener(argv[1], argv[2]);
    if (listener < 0) {
        perror("listen");
        return 1;
    }
    table.sockets[0] = (struct pollfd){listener, POLLIN, 0};
    for (int i = 1; i <= MAX_CONNECTIONS; ++i) table.sockets[i].fd = -1;

    dprintf(1, "Стол готов: %s:%s, ждём %d философов\n", argv[1], argv[2], table.n);
    int result = Run(&table);
    if (stopped) Event(&table, "Остановка по сигналу %d", (int)stopped);
    long long meals = 0;
    for (int i = 0; i < table.n; ++i) {
        meals += table.philosophers[i].meals;
    }
    Event(&table, "Итог: завершили %d/%d, всего приёмов пищи %lld, код %d",
          table.finished, table.n, meals, result);
    for (int i = 1; i <= MAX_CONNECTIONS; ++i) {
        if (table.sockets[i].fd >= 0) ClosePeer(&table, i);
    }
    close(listener);
    return result;
}
