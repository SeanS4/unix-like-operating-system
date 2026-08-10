#include "shell.h"
#include "syscall.h"
#include "string.h"

#define BUFSIZE 1024

void main(int argc, char* argv[]){
    if(argc < 2) _exit();
    if(argv == NULL) _exit();

    int fd = _open(-1, argv[1]);
    if(fd < 0){
        _write(CONSOLEOUT, "invalid file or path\0", 21);
        _exit();
    }
    char buf[BUFSIZE];
    
    int bytes = 1;
    while(bytes != 0){
        bytes = _read(fd, buf, BUFSIZE);
        if(bytes != 0){
            _write(STDOUT, buf, bytes);
        }
    }
    _close(fd);
    _exit();
}