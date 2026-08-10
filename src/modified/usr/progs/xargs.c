#include "shell.h"
#include "syscall.h"
#include "string.h"

#define MAXARGS 8
#define BUFSIZE 1024

static char buf[BUFSIZE];
static char temp[BUFSIZE];

static int is_whitespace(char c) {
    return c == ' ' ||
           c == '\t' ||
           c == '\n' ||
           c == '\r';
}

void main(int argc, char *argv[]) {
    char *new_args[MAXARGS + 1];
    int fixed_args = 0;
    int new_argc = 0;

    if (argv == NULL || argc < 2)
        _exit();

    /*
     * argv[1] is the command xargs will execute.
     * Any later arguments are fixed arguments for that command.
     */
    for (int i = 1; i < argc && fixed_args < MAXARGS; ++i) {
        new_args[fixed_args++] = argv[i];
    }

    new_argc = fixed_args;

    if (fixed_args == 0)
        _exit();

    char *name = new_args[0];

    if (strchr(name, '/') == NULL) {
        snprintf(temp, BUFSIZE, "c/%s", name);
    } else {
        strncpy(temp, name, BUFSIZE);
        temp[BUFSIZE - 1] = '\0';
    }

    new_args[0] = temp;

    /*
     * Validate the target before waiting for standard input.
     */
    int fd = _open(-1, new_args[0]);

    if (fd < 0) {
        const char *error = "invalid path\r\n";
        _write(CONSOLEOUT, error, strlen(error));
        _exit();
    }

    _close(fd);

    /*
     * Read all piped input.
     */
    int total = 0;

    while (total < BUFSIZE - 1) {
        int rc = _read(
            STDIN,
            &buf[total],
            BUFSIZE - 1 - total
        );

        if (rc <= 0)
            break;

        total += rc;
    }

    buf[total] = '\0';

    /*
     * Convert whitespace-separated input into arguments.
     * Include carriage return because echo writes CRLF.
     */
    int in_arg = 0;

    for (int i = 0; i < total && new_argc < MAXARGS; ++i) {
        if (is_whitespace(buf[i])) {
            buf[i] = '\0';
            in_arg = 0;
        } else if (!in_arg) {
            new_args[new_argc++] = &buf[i];
            in_arg = 1;
        }
    }

    if (new_argc == fixed_args)
        _exit();

    new_args[new_argc] = NULL;

    fd = _open(-1, new_args[0]);

    if (fd < 0) {
        const char *error = "invalid path\r\n";
        _write(CONSOLEOUT, error, strlen(error));
        _exit();
    }

    int tid = _fork();

    if (tid < 0) {
        _close(fd);

        const char *error = "failed to fork\r\n";
        _write(CONSOLEOUT, error, strlen(error));
        _exit();
    }

    if (tid == 0) {
        _exec(fd, new_argc, new_args);

        const char *error = "failed to execute command\r\n";
        _write(CONSOLEOUT, error, strlen(error));
        _exit();
    }

    _close(fd);
    _wait(tid);
    _exit();
}