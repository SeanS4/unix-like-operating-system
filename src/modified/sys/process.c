/*! @file process.c
    @brief user process
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

/*!
 * @brief Enables trace messages for process.c
 */
#ifdef PROCESS_TRACE
#define TRACE
#endif

/*!
 * @brief Enables debug messages for process.c
 */
#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "process.h"

#include "conf.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "trap.h"
#include "uio.h"

extern int printf(const char *fmt, ...);

// COMPILE-TIME PARAMETERS
//

/*!
 * @brief Maximum number of processes
 */
#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//

static int build_stack(void* stack, int argc, char** argv);

static void fork_func(struct condition* forked, struct trap_frame* tfr);   

// INTERNAL GLOBAL VARIABLES
//

struct fork_sync {
    struct condition done;
    volatile int ready;
};

/*!
 * @brief The main user process struct
 */
static struct process main_proc;

// static struct process* proctab[NPROC] = {&main_proc};          #####################     IDK WHAT THIS EVEN IS

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert(memory_initialized && heap_initialized);
    assert(!procmgr_initialized);

    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;
}

int process_exec(struct uio* exefile, int argc, char** argv) {
    void *stack = alloc_phys_page();
    struct process *proc = running_thread_process();

    if (proc->uiotab[2] == NULL){
        struct uio *console;
        int rc = open_file("dev", "uart1", &console);
        if (rc < 0)
            return rc;

        proc->uiotab[2] = console;

        uio_addref(console);
        proc->uiotab[0] = console;

        uio_addref(console);
        proc->uiotab[1] = console;
    }

    reset_active_mspace();

    map_page(UMEM_END_VMA - PAGE_SIZE, stack, PTE_R | PTE_W | PTE_U);

    int stacksize = build_stack(stack, argc, argv);
    if (stacksize < 0){
        free_phys_page(stack);
        return stacksize;
    }

    void (*entrypoint)(void) = NULL;
    int rc = elf_load(exefile, &entrypoint);
    if (rc < 0){
        rc = -67;
        free_phys_page(stack);
        return rc;
    }

    uio_close(exefile);

    void *stack_anchor = running_thread_stack_base(); //this returns a pointer to the bottom of the anchor

    struct trap_frame *tfr = (struct trap_frame *)((char *)stack_anchor - sizeof(struct trap_frame)); //this returns a pointer to the bottom address or start of trap_frame

    tfr->a0 = argc;
    tfr->a1 = (uintptr_t)(UMEM_END_VMA - stacksize); //pointer to base of argv that is stored as an int
    tfr->sp = (void *)(UMEM_END_VMA - stacksize); //sp that will be incremented downward
    tfr->sepc = (void *)entrypoint;
    tfr->sstatus |= RISCV_SSTATUS_SPIE; 
    tfr->sstatus |= RISCV_SSTATUS_SUM; 
    tfr->sstatus &= ~RISCV_SSTATUS_SPP;
    tfr->gp = *(void **)((char *)stack_anchor + 8);
    tfr->tp = *(void **)stack_anchor;

    trap_frame_jump(tfr, tfr);
    return 0;
}

int process_fork(const struct trap_frame* tfr) {    
    mtag_t new_mspace = clone_active_mspace();

    struct process *oldproc = running_thread_process();
    struct process *newproc = kcalloc(1, sizeof(struct process)); 
    if (newproc == NULL)
        return -ENOMEM;       

    for (int i = 0; i < PROCESS_UIOMAX; i++){
        newproc->uiotab[i] = oldproc->uiotab[i];
        if (newproc->uiotab[i] != NULL)
            uio_addref(newproc->uiotab[i]);
    }

    newproc->mtag = new_mspace;

    struct trap_frame *tfr_copy = kmalloc(sizeof(struct trap_frame));
    if (tfr_copy == NULL) {
        for (int i = 0; i < PROCESS_UIOMAX; i++){
            if (newproc->uiotab[i] != NULL)
                uio_close(newproc->uiotab[i]);
        }
        kfree(newproc);
        return -ENOMEM;
    }

    *tfr_copy = *tfr;

    struct fork_sync *sync = kcalloc(1, sizeof(struct fork_sync));
    if (sync == NULL) {
        for (int i = 0; i < PROCESS_UIOMAX; i++){
            if (newproc->uiotab[i] != NULL)
                uio_close(newproc->uiotab[i]);
        }
        kfree(tfr_copy);
        kfree(newproc);
        return -ENOMEM;
    }

    condition_init(&sync->done, "fork_done");
    sync->ready = 0;

    int tid = spawn_thread(NULL, (void (*)(void))fork_func, &sync->done, tfr_copy);

    if(tid < 0){
        for (int i = 0; i < PROCESS_UIOMAX; i++){
            if (newproc->uiotab[i] != NULL)
                uio_close(newproc->uiotab[i]);
        }
        kfree(sync);
        kfree(tfr_copy);
        kfree(newproc);
        return tid;
    }

    newproc->tid = tid;

    thread_set_process(tid, newproc);

    while (!sync->ready)
        condition_wait(&sync->done);

    kfree(sync);

    return tid;
}

/** \brief
 *
 *
 *  Discard memory space, close your associated uio, free the memory you're supposed to free.
 *
 *
 */
void process_exit(void) {
    // FIXME
    struct process *proc = running_thread_process();

    for (int i = 0; i < PROCESS_UIOMAX; i++){
        if (proc->uiotab[i] != NULL){
            uio_close(proc->uiotab[i]);
            proc->uiotab[i] = NULL;
        }
    }

    discard_active_mspace();

    running_thread_exit();
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * \brief Builds the initial user stack for a new process.
 *
 * Builds the stack for a new process, including the argument vector (\p argv)
 * and the strings it points to. Note that \p argv must contain \p argc + 1
 * elements (the last one is a NULL pointer).
 *
 * Remember to round the final stack size up to a multiple of 16 bytes
 * (RISC-V ABI requirement).
 *
 * \param[in,out] stack  Pointer to the stack page (destination buffer).
 * \param[in]     argc   Number of arguments in \p argv.
 * \param[in]     argv   Array of argument pointers; length is \p argc+1 and
 *                       \p argv[argc] must be NULL.
 *
 * \return Size of the stack page on success; negative error code on failure.
 */
int build_stack(void* stack, int argc, char** argv) {
    size_t stksz, argsz;
    uintptr_t* newargv;
    char* p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc) return -ENOMEM;

    stksz = (argc + 1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i]) + 1;
        if (PAGE_SIZE - stksz < argsz) return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert(stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv + argc + 1);

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i]) + 1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

// /**
//  * \brief Function to be executed by the child process after fork.
//  * This is a very beautiful function.
//  * Tell the parent process that it is done with the trap frame, then jumps to user space (hint:
//  * which function should we use?)
//  *
//  * \param[in] done  Pointer to a condition variable to signal parent
//  * \param[in] tfr   Pointer to a trap frame
//  *
//  * \return NONE (very important, this is a hint)
//  */
void fork_func(struct condition* done, struct trap_frame* tfr) {
    struct fork_sync *sync = (struct fork_sync *)done;

    tfr->a0 = 0;

    sync->ready = 1;
    condition_broadcast(&sync->done);

    uintptr_t sp;
    asm volatile ("mv %0, sp" : "=r"(sp));

    struct trap_frame *jump_tfr =
        (struct trap_frame *)(sp - sizeof(struct trap_frame) - 16);

    volatile unsigned long *dst = (volatile unsigned long *)jump_tfr;
    volatile unsigned long *src = (volatile unsigned long *)tfr;

    for (unsigned int i = 0; i < sizeof(struct trap_frame) / sizeof(unsigned long); i++)
        dst[i] = src[i];

    void *stack_anchor = running_thread_stack_base();
    void *reentry_tfr = (char *)stack_anchor - sizeof(struct trap_frame);

    trap_frame_jump(jump_tfr, reentry_tfr);
}