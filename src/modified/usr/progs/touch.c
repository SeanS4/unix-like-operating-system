#include "shell.h"
#include "syscall.h"
#include "string.h"

void main(int argc, char* argv[]){
    if(argv == NULL || argc < 2) _exit();
    for(int i = 1; i < argc; ++i){
        _fscreate(argv[i]);
    }
    _exit();
}