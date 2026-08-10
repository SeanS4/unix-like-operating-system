#include "console.h"
#include "filesys.h"
#include "string.h"     
#include "process.h"
#include "uio.h"
#include "conf.h"       
#include "error.h"
#include "trap.h"
#include "scnum.h"
#include "memory.h"
#include "riscv.h"

extern void handle_syscall(struct trap_frame *tfr);

int test_process_exec(void) {
    struct trap_frame tfr;
    const char *mp = "c";
    const char *fl = "shell";
    struct uio *u;

    int rc = open_file(mp, fl, &u);
    if (rc < 0) {
        return rc;
    }

    struct process *proc = running_thread_process();
    int newfd = -1;

    for(int i = 0; i < PROCESS_UIOMAX; i++){
        if(proc->uiotab[i] == NULL){
            newfd = i;
            break;
        }
    }

    proc->uiotab[newfd] = u;

    if (newfd == -1)
        return -55; //signifies no open uiotabs

    tfr.a7 = SYSCALL_EXEC;
    tfr.a0 = newfd;
    tfr.a1 = 0;
    tfr.a2 = (unsigned long)NULL;
    handle_syscall(&tfr);

    return 0;
}

void run_testsuite_1(void) {
        int rc = test_process_exec();
        kprintf("Test result: %s (rc=%d, %s)\n",
            (rc == 0) ? "PASS" : "FAIL", 
            rc, 
            error_name(rc));
}