#ifndef HW4_NET_H
#define HW4_NET_H

#include <stdint.h>

enum { MAX_PHILOSOPHERS = 64, MAX_TIME_MS = 86400000 };

// Первый байт от клиента: 0 — наблюдатель, 1..64 — номер философа плюс один.
// После регистрации философ и стол обмениваются командами по одному байту.
enum {
    HUNGRY = 'H',
    RELEASE = 'R',
    THINK = 'T',
    EAT = 'E',
    FINISH = 'F',
};

int Number(const char *text, uint64_t max, uint64_t *value);
int OpenListener(const char *ip, const char *port);
int Connect(const char *ip, const char *port);
int SetNonblocking(int fd);
int SendByte(int fd, unsigned char byte);
long long NowMs(void);

#endif
