#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int Number(const char *text, uint64_t max, uint64_t *value) {
    if (*text < '0' || *text > '9') return 0;
    char *end;
    errno = 0;
    unsigned long long result = strtoull(text, &end, 10);
    if (errno || *end || result > max) return 0;
    *value = result;
    return 1;
}

static int OpenSocket(const char *ip, const char *port, struct sockaddr_in *address) {
    uint64_t number;
    struct in_addr host;
    if (!Number(port, 65535, &number) || number == 0 || inet_pton(AF_INET, ip, &host) != 1) {
        errno = EINVAL;
        return -1;
    }
    *address = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)number),
        .sin_addr = host,
    };
    return socket(AF_INET, SOCK_STREAM, 0);
}

static int CloseOnError(int fd) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

int OpenListener(const char *ip, const char *port) {
    struct sockaddr_in address;
    int fd = OpenSocket(ip, port, &address);
    if (fd < 0) return -1;
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0 &&
        bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
        listen(fd, SOMAXCONN) == 0 && SetNonblocking(fd) == 0) {
        return fd;
    }
    return CloseOnError(fd);
}

int Connect(const char *ip, const char *port) {
    struct sockaddr_in address;
    int fd = OpenSocket(ip, port, &address);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        return fd;
    }
    return CloseOnError(fd);
}

int SetNonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int SendByte(int fd, unsigned char byte) {
    ssize_t sent;
    do {
        sent = send(fd, &byte, 1, 0);
    } while (sent < 0 && errno == EINTR);
    return sent == 1;
}

long long NowMs(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}
