#include "shell.h"
#include "syscall.h"
#include "string.h"

#define BUFSIZE 1024

void main(int argc, char* argv[]) {
    if (argv == NULL) _exit();

    char buf[BUFSIZE];

    int bytes = 1;
    int lines = 0;
    int words = 0;
    int bytes_total = 0;
    char in_word = 0;

    if (argc < 2) {
        while (bytes != 0) {
            bytes = _read(STDIN, buf, BUFSIZE);
            bytes_total += bytes;

            for (int i = 0; i < bytes; ++i) {
                char c = buf[i];

                if (c == '\n') {
                    lines++;
                }

                if (c == ' ' || c == '\n' || c == '\t') {
                    in_word = 0;
                } else if (!in_word) {
                    words++;
                    in_word = 1;
                }
            }
        }
    } else {
        int fd = _open(-1, argv[1]);

        if (fd < 0) {
            _write(CONSOLEOUT, "invalid file or path\r\n", 22);
            _exit();
        }

        while (bytes != 0) {
            bytes = _read(fd, buf, BUFSIZE);
            bytes_total += bytes;

            for (int i = 0; i < bytes; ++i) {
                char c = buf[i];

                if (c == '\n') {
                    lines++;
                }

                if (c == ' ' || c == '\n' || c == '\t') {
                    in_word = 0;
                } else if (!in_word) {
                    words++;
                    in_word = 1;
                }
            }
        }

        _close(fd);
    }

    char result[BUFSIZE];
    snprintf(result, BUFSIZE, "%d\t%d\t%d\r\n",
             lines, words, bytes_total);

    _write(STDOUT, result, strlen(result));
    _exit();
}