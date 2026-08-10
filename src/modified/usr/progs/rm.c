#include "shell.h"
#include "syscall.h"
#include "string.h"

void main(int argc, char *argv[]) {
    if (argv == NULL || argc < 2)
        _exit();

    for (int i = 1; i < argc; ++i) {
        int rc = _fsdelete(argv[i]);

        if (rc < 0) {
            const char *error = "Couldn't delete the given file\r\n";
            _write(CONSOLEOUT, error, strlen(error));
        }
    }

    _exit();
}