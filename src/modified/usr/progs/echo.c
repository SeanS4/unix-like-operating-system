#include "shell.h"
#include "syscall.h"
#include "string.h"

void main(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        _write(STDOUT, argv[i], strlen(argv[i]));

        if (i + 1 < argc)
            _write(STDOUT, " ", 1);
    }

    _write(STDOUT, "\r\n", 2);
    _exit();
}