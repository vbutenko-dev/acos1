#include "net.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        dprintf(1, "Использование: %s IP PORT\n", argv[0]);
        return argc == 2 && strcmp(argv[1], "-h") == 0 ? 0 : 2;
    }
    signal(SIGPIPE, SIG_IGN);
    int fd = Connect(argv[1], argv[2]);
    if (fd < 0) {
        perror("connect");
        return 1;
    }
    if (!SendByte(fd, 0)) {
        perror("send");
        close(fd);
        return 1;
    }
    char buffer[1024];
    int result = 0;
    while (1) {
        ssize_t size = read(fd, buffer, sizeof(buffer));
        if (size == 0) break;
        if (size < 0 && errno == EINTR) continue;
        if (size < 0) {
            perror("read");
            result = 1;
            break;
        }
        if (fwrite(buffer, 1, (size_t)size, stdout) != (size_t)size ||
            fflush(stdout) == EOF) {
            perror("stdout");
            result = 1;
            break;
        }
    }
    close(fd);
    return result;
}
