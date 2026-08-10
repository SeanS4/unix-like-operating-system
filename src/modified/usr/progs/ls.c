#include "shell.h"
#include "syscall.h"
#include "string.h"

#define BUFSIZE 128

static void print_message(const char *msg) {
    _write(CONSOLEOUT, msg, strlen(msg));
}

void main(int argc, char *argv[]) {
    char path[8];

    if (argv == NULL || argc != 2) {
        print_message(
            "usage: ls c displays commands, "
            "ls dev displays devices, and "
            "ls / displays root\r\n"
        );
        _exit();
    }

    if (strcmp(argv[1], "c") == 0) {
        strncpy(path, "c/", sizeof(path));
    } else if (strcmp(argv[1], "dev") == 0) {
        strncpy(path, "dev/", sizeof(path));
    } else if (strcmp(argv[1], "/") == 0) {
        strncpy(path, "/", sizeof(path));
    } else {
        print_message("invalid file or path\r\n");
        _exit();
    }

    path[sizeof(path) - 1] = '\0';

    int fd = _open(-1, path);
    if (fd < 0) {
        print_message("invalid file or path\r\n");
        _exit();
    }

    char buf[BUFSIZE];
    int bytes;

    while ((bytes = _read(fd, buf, BUFSIZE - 1)) > 0) {
        _write(STDOUT, buf, bytes);
        _write(STDOUT, "\r\n", 2);
    }

    _close(fd);
    _exit();
}