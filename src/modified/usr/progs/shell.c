#include "syscall.h"
#include "string.h"
#include "shell.h"

#define BUFSIZE 1024
#define MAXARGS 8

static void console_message(const char *msg) {
    _write(CONSOLEOUT, msg, strlen(msg));
}

/*
 * Some versions of parse_path modify the supplied string in place.
 * Always pass a writable scratch copy to path-based system calls.
 */
static void copy_path(char *dst, const char *src) {
    strncpy(dst, src, BUFSIZE);
    dst[BUFSIZE - 1] = '\0';
}

static int open_path(int fd, const char *path) {
    char scratch[BUFSIZE];

    copy_path(scratch, path);
    return _open(fd, scratch);
}

static int create_path(const char *path) {
    char scratch[BUFSIZE];

    copy_path(scratch, path);
    return _fscreate(scratch);
}

static int delete_path(const char *path) {
    char scratch[BUFSIZE];

    copy_path(scratch, path);
    return _fsdelete(scratch);
}

static void build_command_path(char *dst, const char *command) {
    if (strchr(command, '/') == NULL) {
        snprintf(dst, BUFSIZE, "c/%s", command);
    } else {
        copy_path(dst, command);
    }
}

char *find_terminator(char *buf) {
    char *p = buf;

    while (*p) {
        switch (*p) {
            case ' ':
            case '\0':
            case FIN:
            case FOUT:
            case PIPE:
                return p;

            default:
                p++;
                break;
        }
    }

    return p;
}

int parse(char *buf, char **argv, int *in, int *out, int *pipe) {
    int argc = 0;
    int pipe_count = 0;
    char temp;
    char first_char = '\0';
    char *head = buf;
    char *end;

    *in = 0;
    *out = 0;

    for (;;) {
        if (argc == MAXARGS)
            return -1;

        while (*head == ' ')
            head++;

        if (*head == '\0') {
            argv[argc] = NULL;
            return argc;
        }

        argv[argc++] = head;
        end = find_terminator(head);

        if (argv[argc - 1] != NULL)
            first_char = *argv[argc - 1];

        temp = *end;
        *end = '\0';

        switch (temp) {
            case ' ':
                head = end + 1;
                break;

            case '\0':
                argv[argc] = NULL;
                return argc;

            case FOUT:
                if (first_char == FOUT) {
                    *out = argc - 1;
                    argv[argc - 1] = NULL;
                } else {
                    if (argc == MAXARGS)
                        return -1;

                    *out = argc;
                    argv[argc++] = NULL;
                }

                head = end + 1;
                break;

            case FIN:
                if (first_char == FIN) {
                    *in = argc - 1;
                    argv[argc - 1] = NULL;
                } else {
                    if (argc == MAXARGS)
                        return -1;

                    *in = argc;
                    argv[argc++] = NULL;
                }

                head = end + 1;
                break;

            case PIPE:
                if (pipe_count >= MAXARGS)
                    return -1;

                /*
                 * When "|" is separated by spaces, it was temporarily
                 * added as an argument. Remove it so it does not count
                 * against MAXARGS.
                 */
                if (first_char == PIPE)
                    argc--;

                pipe[pipe_count++] = argc;
                argv[argc] = NULL;
                head = end + 1;
                break;

            default:
                *end = temp;
                break;
        }
    }
}

static int redirect_input(const char *path) {
    int fd = open_path(-1, path);

    if (fd < 0) {
        console_message("failed to open input file\r\n");
        return -1;
    }

    _close(STDIN);

    int rc = _uiodup(fd, STDIN);
    _close(fd);

    if (rc != STDIN) {
        console_message("failed to redirect input\r\n");
        return -1;
    }

    return 0;
}

static int redirect_output(const char *path) {
    int fd = open_path(-1, path);

    /*
     * Output redirection should overwrite an existing regular file.
     * Close and recreate it so stale bytes cannot remain at the end.
     */
    if (fd >= 0) {
        _close(fd);

        if (delete_path(path) < 0) {
            console_message("failed to replace output file\r\n");
            return -1;
        }
    }

    if (create_path(path) < 0) {
        console_message("failed to create output file\r\n");
        return -1;
    }

    fd = open_path(-1, path);
    if (fd < 0) {
        console_message("failed to open output file\r\n");
        return -1;
    }

    _close(STDOUT);

    int rc = _uiodup(fd, STDOUT);
    _close(fd);

    if (rc != STDOUT) {
        console_message("failed to redirect output\r\n");
        return -1;
    }

    return 0;
}

static void execute_single(int argc, char **argv, int in, int out) {
    char command_path[BUFSIZE];
    int fd;
    int tid;

    build_command_path(command_path, argv[0]);

    fd = open_path(-1, command_path);
    if (fd < 0) {
        console_message("invalid path\r\n");
        return;
    }

    argv[0] = command_path;

    tid = _fork();
    if (tid < 0) {
        _close(fd);
        console_message("failed to fork\r\n");
        return;
    }

    if (tid == 0) {
        if (in != 0) {
            if (argv[in + 1] == NULL || redirect_input(argv[in + 1]) < 0)
                _exit();
        }

        if (out != 0) {
            if (argv[out + 1] == NULL || redirect_output(argv[out + 1]) < 0)
                _exit();
        }

        int exec_argc = argc;

        if (in != 0 && in < exec_argc)
            exec_argc = in;

        if (out != 0 && out < exec_argc)
            exec_argc = out;

        argv[exec_argc] = NULL;

        _exec(fd, exec_argc, argv);
        console_message("failed to execute command\r\n");
        _exit();
    }

    _close(fd);
    _wait(tid);
}

static void execute_pipe(int argc, char **argv, int *pipe) {
    int pipe_index = pipe[0];
    int right_start = pipe_index;
    int left_argc = pipe_index;
    int right_argc = argc - right_start;

    char left_path[BUFSIZE];
    char right_path[BUFSIZE];

    int left_fd;
    int right_fd;
    int wfd = -1;
    int rfd = -1;
    int left_tid;
    int right_tid;

    if (pipe[1] != 0) {
        console_message("only one pipe is supported\r\n");
        return;
    }

    if (left_argc <= 0 || right_argc <= 0 ||
        argv[0] == NULL || argv[right_start] == NULL) {
        console_message("invalid pipe syntax\r\n");
        return;
    }

    build_command_path(left_path, argv[0]);
    build_command_path(right_path, argv[right_start]);

    left_fd = open_path(-1, left_path);
    if (left_fd < 0) {
        console_message("invalid left command path\r\n");
        return;
    }

    right_fd = open_path(-1, right_path);
    if (right_fd < 0) {
        _close(left_fd);
        console_message("invalid right command path\r\n");
        return;
    }

    if (_pipe(&wfd, &rfd) < 0) {
        _close(left_fd);
        _close(right_fd);
        console_message("failed to create pipe\r\n");
        return;
    }

    left_tid = _fork();
    if (left_tid < 0) {
        _close(wfd);
        _close(rfd);
        _close(left_fd);
        _close(right_fd);
        console_message("failed to fork left command\r\n");
        return;
    }

    if (left_tid == 0) {
        _close(right_fd);
        _close(rfd);

        _close(STDOUT);
        int rc = _uiodup(wfd, STDOUT);
        _close(wfd);

        if (rc != STDOUT) {
            console_message("failed to connect pipe output\r\n");
            _exit();
        }

        argv[0] = left_path;
        argv[left_argc] = NULL;

        _exec(left_fd, left_argc, argv);
        console_message("failed to execute left command\r\n");
        _exit();
    }

    right_tid = _fork();
    if (right_tid < 0) {
        _close(wfd);
        _close(rfd);
        _close(left_fd);
        _close(right_fd);
        _wait(left_tid);
        console_message("failed to fork right command\r\n");
        return;
    }

    if (right_tid == 0) {
        _close(left_fd);
        _close(wfd);

        _close(STDIN);
        int rc = _uiodup(rfd, STDIN);
        _close(rfd);

        if (rc != STDIN) {
            console_message("failed to connect pipe input\r\n");
            _exit();
        }

        argv[right_start] = right_path;
        argv[argc] = NULL;

        _exec(right_fd, right_argc, &argv[right_start]);
        console_message("failed to execute right command\r\n");
        _exit();
    }

    _close(wfd);
    _close(rfd);
    _close(left_fd);
    _close(right_fd);

    _wait(left_tid);
    _wait(right_tid);
}

int main(void) {
    char buf[BUFSIZE];
    char *argv[MAXARGS + 1];

    /*
     * Keep the original known-working console initialization sequence.
     * Do not terminate the shell based on these return values.
     */
    open_path(CONSOLEOUT, "dev/uart1");

    _close(STDIN);
    _uiodup(CONSOLEOUT, STDIN);

    _close(STDOUT);
    _uiodup(CONSOLEOUT, STDOUT);

    printf("Starting RISC-V Shell\r\n");

    for (;;) {
        int in = 0;
        int out = 0;
        int pipe[MAXARGS + 1];
        int argc;

        memset(pipe, 0, sizeof(pipe));

        printf("RISC-V OS> ");
        getsn(buf, BUFSIZE - 1);

        argc = parse(buf, argv, &in, &out, pipe);

        if (argc == 0)
            continue;

        if (argc < 0) {
            console_message("too many arguments\r\n");
            continue;
        }

        if (argv[0] == NULL) {
            console_message("invalid command\r\n");
            continue;
        }

        if (strcmp(argv[0], "exit") == 0)
            _exit();

        if (pipe[0] != 0) {
            if (in != 0 || out != 0) {
                console_message(
                    "redirection with a pipe is not supported\r\n"
                );
                continue;
            }

            execute_pipe(argc, argv, pipe);
        } else {
            execute_single(argc, argv, in, out);
        }
    }
}