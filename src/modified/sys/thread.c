// thread.c - Threads
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

/*! @file thread.c
    @brief Thread manager and operations
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef THREAD_TRACE
#define TRACE
#endif

#ifdef THREAD_DEBUG
#define DEBUG
#endif
#include <stddef.h>
#include "see.h"


#include "thread.h"

#include <stddef.h>
#include <stdint.h>

#include "misc.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "error.h"

#include <stdarg.h>

#include "memory.h"
#include "process.h"

// COMPILE-TIME PARAMETERS
//

// NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

// EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//


enum thread_state {
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_SELF,
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context {
    union {
        uint64_t s[12];
        struct {
            uint64_t a[8];      // s0 .. s7
            void (*pc)(void);   // s8
            uint64_t _pad;      // s9
            void * fp;          // s10
            void * ra;          // s11
        } startup;
    };

    void * ra;
    void * sp;
};

struct thread_stack_anchor {
    struct thread * ktp;
    void * kgp;
};

struct thread {
    struct thread_context ctx;  // must be first member (thrasm.s)
    int id; // index into thrtab[]
    enum thread_state state;
    const char * name;
    struct thread_stack_anchor * stack_anchor;
    void * stack_lowest;
    struct process * proc;
    struct thread * parent;
    struct thread * list_next;
    struct condition * wait_cond;
    struct condition child_exit;
    struct lock * lock_list;
};

// INTERNAL MACRO DEFINITIONS
// 

// Pointer to running thread, which is kept in the tp (x4) register.

#define TP ((struct thread*)__builtin_thread_pointer())

// Macro for changing thread state. If compiled for debugging (DEBUG is
// defined), prints function that changed thread state.

#define set_thread_state(t,s) do { \
    debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
        (t)->name, (t)->id, \
        thread_state_name((t)->state), \
        thread_state_name(s), \
        TP->name, TP->id, \
        __func__); \
    (t)->state = (s); \
} while (0)

// INTERNAL FUNCTION DECLARATIONS
//

// Initializes the main and idle threads. called from threads_init().

static void init_main_thread(void);
static void init_idle_thread(void);

// Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread * thr);

// Returns a string representing the state name. Used by debug and trace
// statements, so marked unused to avoid compiler warnings.

static const char * thread_state_name(enum thread_state state)
    __attribute__ ((unused));

// void thread_reclaim(int tid)
//
// Reclaims a thread's slot in thrtab and makes its parent the parent of its
// children. Frees the struct thread of the thread.

static void thread_reclaim(int tid);

// struct thread * create_thread(const char * name)
//
// Creates and initializes a new thread structure. The new thread is not added
// to any list and does not have a valid context (_thread_switch cannot be
// called to switch to the new thread).

static struct thread * create_thread(const char * name);

// void running_thread_suspend(void)
// Suspends the currently running thread and resumes the next thread on the
// ready-to-run list using _thread_swtch (in threasm.s). Must be called with
// interrupts enabled. Returns when the current thread is next scheduled for
// execution. If the current thread is TP, it is marked READY and placed
// on the ready-to-run list. Note that running_thread_suspend will only return if the
// current thread becomes READY.

static void running_thread_suspend(void);

void lock_release_completely(struct lock * lock);

// void release_all_thread_locks(struct thread * thr)
// Releases all locks held by a thread. Called when a thread exits.

static void release_all_thread_locks(struct thread * thr);

// The following functions manipulate a thread list (struct thread_list). Note
// that threads form a linked list via the list_next member of each thread
// structure. Thread lists are used for the ready-to-run list (ready_list) and
// for the list of waiting threads of each condition variable. These functions
// are not interrupt-safe! The caller must disable interrupts before calling any
// thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list * list);
static int tlempty(const struct thread_list * list);
static void tlinsert(struct thread_list * list, struct thread * thr);
static struct thread * tlremove(struct thread_list * list);
static void tlappend(struct thread_list * l0, struct thread_list * l1);

static void idle_thread_func(void);

// IMPORTED FUNCTION DECLARATIONS
// defined in thrasm.s
//

extern struct thread * _thread_swtch(struct thread * thr);

extern void _thread_startup(void);

// INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR-1)

static struct thread main_thread;
static struct thread idle_thread;

extern char _main_stack_lowest[]; // from start.s
extern char _main_stack_anchor[]; // from start.s

static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_SELF,
    .stack_anchor = (void*)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit"
};

extern char _idle_stack_lowest[]; // from thrasm.s
extern char _idle_stack_anchor[]; // from thrasm.s

static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void*)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = &_thread_startup,
    // FIXME your code goes here
    .ctx.startup.pc = idle_thread_func
    
};

static struct thread * thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread
};

static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread
};

// EXPORTED FUNCTION DEFINITIONS
//


int running_thread(void) {
    return TP->id;
}

void thrmgr_init(void) {
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}
//int spawn_thread(const char *name, void (*entry)(void), ...)
//Inputs:
//const char *name - thread name.
//void (*entry)(void) - Function the new thread will run.
//- Optional arguments (up to 8) passed to entry
//when the thread starts; each is treated as a
//64-bit value (uint64_t) in start context.
//Outputs:
//int - Thread id (tid) ≥ 0 on success; negative errno
//on failure
//Description:
//Creates a new kernel thread, initializes its context so that it begins
//execution at entry with the supplied variadic arguments, and enqueues
//it on the ready list using a simple round-robin scheduler.
//Side Effects:
//Allocates a thread structure and stack
//modifies global ready list and scheduler state.
int spawn_thread (
    const char * name,
    void (*entry)(void),
    ...)
{
    struct thread * child;
    va_list ap;
    int pie;
    int i;

    child = create_thread(name);

    if (child == NULL)
        return -EMTHR;

    set_thread_state(child, THREAD_READY);

    pie = disable_interrupts();
    tlinsert(&ready_list, child);
    restore_interrupts(pie);

   // FIXME your code goes here
   // filling in entry function arguments is given below, the rest is up to you
    pie = disable_interrupts();
    va_start(ap, entry);
    for (i = 0; i < 8; i++)
        child->ctx.startup.a[i] = va_arg(ap, uint64_t);
    va_end(ap);
    child->ctx.sp = child->stack_anchor;
    child->ctx.ra = &_thread_startup;
    child->ctx.startup.pc = entry;
    restore_interrupts(pie);
    return child->id;
}

//void running_thread_exit(void)
//Inputs:
//None
//Outputs:
//does not return
//Description:
//Terminates the currently running thread. If the running thread is the main
//thread, it halts the system with success. Otherwise it releases any locks
//held by the thread, marks the thread state as THREAD_EXITED, signals the
//parent’s child_exit condition (if a parent exists), and then calls
//running_thread_suspend() to yield control. Execution should never return
//from this function; if it does, the kernel halts with failure.
//Side Effects:
//Modifies the running thread’s state, broadcasts on a condition variable,
//triggers a context switch, and may halt the machine.
void running_thread_exit(void) {
     // FIXME your code goes here
     if (TP->id == MAIN_TID)
    	halt_success();

    release_all_thread_locks(TP); 
    set_thread_state(TP, THREAD_EXITED);

    if (TP->parent)
        condition_broadcast(&TP->parent->child_exit);

    running_thread_suspend();

    halt_failure(); // should be unreachable

}


void running_thread_yield(void) {
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}
//int thread_join(int tid)
//Inputs:
//int tid — If non-zero, wait for the specific child thread with ID tid.
//If zero, wait for any child of the calling thread to exit.
//Outputs:
//Returns the TID of the child that exited on success.
//Returns -EINVAL if:
//tid != 0 and the thread does not exist or is not a child, or
//tid == 0 and the caller has no children.
//Description:
//Waits for a child of the calling (running) thread to exit.
//If the requested child has already exited, returns immediately after
//reclaiming the child’s resources. Otherwise, the caller waits on its
//child_exit condition and is awakened when a child exits, then reclaims
//that child and returns its TID.
//Side Effects:
//May block the calling thread; reclaims (frees) a child’s resources.
int thread_join(int tid) {
    // FIXME your code goes here
    if (tid != 0) {
    	if (tid <= 0 || tid >= NTHR || tid == IDLE_TID) 
		return -EINVAL;
    	if (!thrtab[tid] || thrtab[tid]->parent != TP) 
		return -EINVAL;

        for (;;) {
            int pie = disable_interrupts();
            struct thread *c = thrtab[tid];
            if (!c || c->parent != TP) { 
		    restore_interrupts(pie); 
		    return -EINVAL; 
	    }
            if (c->state == THREAD_EXITED) { 
		    restore_interrupts(pie); 
		    thread_reclaim(tid); 
		    return tid; 
	    }
            condition_wait(&TP->child_exit);
            restore_interrupts(pie);
        }
    } else {
        int i, have_child = 0;
        for (i = 1; i < IDLE_TID; i++) {
            if (thrtab[i] && thrtab[i]->parent == TP) {
                have_child = 1;
                if (thrtab[i]->state == THREAD_EXITED) { 
			thread_reclaim(i); 
			return i; 
		}
            }
        }
        if (!have_child) 
		return -EINVAL;

        for (;;) {
            int pie = disable_interrupts();
            for (i = 1; i < IDLE_TID; i++) {
                struct thread *c = thrtab[i];
                if (c && c->parent == TP && c->state == THREAD_EXITED) {
                    restore_interrupts(pie); 
		    thread_reclaim(i); 
		    return i;
                }
            }
            condition_wait(&TP->child_exit);
            restore_interrupts(pie);
        }
    }
}




struct process * thread_process(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->proc;
}

struct process * running_thread_process(void) {
    return TP->proc;
}

void thread_set_process(int tid, struct process * proc) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    thrtab[tid]->proc = proc;
}

void thread_detach(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    thrtab[tid]->parent = NULL;
}

const char * thread_name(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

const char * running_thread_name(void) {
    return TP->name;
}

void * running_thread_stack_base(void){
    return TP->stack_anchor;
}

void condition_init(struct condition * cond, const char * name) {
    tlclear(&cond->wait_list);
    cond->name = name;
}

void condition_wait(struct condition * cond) {
    int pie;

    trace("%s(cond=<%s>) in <%s:%d>", __func__,
        cond->name, TP->name, TP->id);

    assert(TP->state == THREAD_SELF);

    // Insert current thread into condition wait list
    
    set_thread_state(TP, THREAD_WAITING);
    TP->wait_cond = cond;
    TP->list_next = NULL;

    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);

    running_thread_suspend();
}
//condition_broadcast(struct condition *cond)
//Inputs: struct condition *cond — condition variable to wake all waiters on
//Outputs: None
//Description: Marks every thread waiting on cond as THREAD_READY and enqueues them on the
//ready-to-run list (FIFO order)
//Side Effects: empties cond->wait_list, updates each waiter’s state,
//and appends to the global ready list.
void condition_broadcast(struct condition * cond) {
    // FIXME your code goes here
    int pie = disable_interrupts();

    struct thread *t = cond->wait_list.head;
    while (t) {
        set_thread_state(t, THREAD_READY);
        t->wait_cond = NULL;
        t = t->list_next;
    }
    tlappend(&ready_list, &cond->wait_list);   // moves all waiters, empties cond->wait_list

    restore_interrupts(pie);
}

void lock_init(struct lock * lock) {
    memset(lock, 0, sizeof(struct lock));
    condition_init(&lock->release, "lock_release");
}

void lock_acquire(struct lock * lock) {
    if (lock->owner != TP) {
        while (lock->owner != NULL)
            condition_wait(&lock->release);
        
        lock->owner = TP;
        lock->cnt = 1;
        lock->next = TP->lock_list;
        TP->lock_list = lock;
    } else
        lock->cnt += 1;
}

void lock_release(struct lock * lock) {
    assert (lock->owner == TP);
    assert (lock->cnt != 0);

    lock->cnt -= 1;

    if (lock->cnt == 0)
        lock_release_completely(lock);
}

// INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void) {
    // Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
}

void init_idle_thread(void) {
    // Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
}

static void set_running_thread(struct thread * thr) {
    asm inline ("mv tp, %0" :: "r"(thr) : "tp");
}

const char * thread_state_name(enum thread_state state) {
    static const char * const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_SELF] = "SELF",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"
    };

    if (0 <= (int)state && (int)state < sizeof(names)/sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void thread_reclaim(int tid) {
    struct thread * const thr = thrtab[tid];
    int ctid;

    assert (0 < tid && tid < NTHR && thr != NULL);
    assert (thr->state == THREAD_EXITED);

    // Make our parent thread the parent of our child threads. We need to scan
    // all threads to find our children. We could keep a list of all of a
    // thread's children to make this operation more efficient.

    for (ctid = 1; ctid < NTHR; ctid++) {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;
    kfree(thr);
}

struct thread * create_thread(const char * name) {
    struct thread_stack_anchor * anchor;
    void * stack_lowest;
    size_t stack_size;
    struct thread * thr;
    int tid;

    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);

    // Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;
    
    if (tid == NTHR)
        return NULL;
    
    // Allocate a struct thread and a stack

    thr = kcalloc(1, sizeof(struct thread));
    
    stack_size = PAGE_SIZE; // change to PAGE_SIZE in mp3
    stack_lowest = alloc_phys_page();
    anchor = stack_lowest + stack_size;
    anchor -= 1; // anchor is at base of stack
    thr->stack_lowest = stack_lowest;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;

    thrtab[tid] = thr;

    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    thr->proc = TP->proc;
    return thr;
}
//running_thread_suspend(void)
//Inputs: None
//Outputs: None
//Description: Round-robin scheduler step. If TP is still runnable (THREAD_SELF),
//mark it THREAD_READY and enqueue it at the tail of ready_list.
//Dequeue the next thread from ready_list, mark it THREAD_SELF,
//and context-switch to it. When this thread is later scheduled again,
//execution resumes after the switch.
//Side Effects:may free the previous thread’s stack if it had THREAD_EXITED; changes global
//ready_list and per-thread state.
void running_thread_suspend(void) {
    // FIXME your code goes here
    struct thread *next, *prev;
    int pie = disable_interrupts();

    if (TP->state == THREAD_SELF) {
        set_thread_state(TP, THREAD_READY);
        tlinsert(&ready_list, TP);
    }
    next = tlremove(&ready_list);
    restore_interrupts(pie);

    if (next->proc != NULL)              
        switch_mspace(next->proc->mtag);   //enters correct mspace if thread has an associated process

    set_thread_state(next, THREAD_SELF);
    pie = enable_interrupts();
    prev = _thread_swtch(next);
    restore_interrupts(pie);
    if (prev && prev->state == THREAD_EXITED) {
        if (prev->stack_lowest) {
            free_phys_page(prev->stack_lowest);
            prev->stack_lowest = NULL;
            prev->stack_anchor = NULL;
        }
    }
}
	

void tlclear(struct thread_list * list) {
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list * list) {
    return (list->head == NULL);
}

void tlinsert(struct thread_list * list, struct thread * thr) {
    thr->list_next = NULL;

    if (thr == NULL)
        return;

    if (list->tail != NULL) {
        assert (list->head != NULL);
        list->tail->list_next = thr;
    } else {
        assert(list->head == NULL);
        list->head = thr;
    }

    list->tail = thr;
}

struct thread * tlremove(struct thread_list * list) {
    struct thread * thr;

    thr = list->head;
    
    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;
    
    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

void tlappend(struct thread_list * l0, struct thread_list * l1) {
    if (l0->head != NULL) {
        assert(l0->tail != NULL);
        
        if (l1->head != NULL) {
            assert(l1->tail != NULL);
            l0->tail->list_next = l1->head;
            l0->tail = l1->tail;
        }
    } else {
        assert(l0->tail == NULL);
        l0->head = l1->head;
        l0->tail = l1->tail;
    }

    l1->head = NULL;
    l1->tail = NULL;
}

void lock_release_completely(struct lock * lock) {
    struct lock ** hptr;

    condition_broadcast(&lock->release);
    hptr = &TP->lock_list;
    while (*hptr != lock && *hptr != NULL)
        hptr = &(*hptr)->next;
    assert (*hptr != NULL);
    *hptr = (*hptr)->next;
    lock->owner = NULL;
    lock->next = NULL;
}

void release_all_thread_locks(struct thread * thr) {
    struct lock * head;
    struct lock * next;

    head = thr->lock_list;

    while (head != NULL) {
        next = head->next;
        head->next = NULL;
        head->owner = NULL;
        head->cnt = 0;
        condition_broadcast(&head->release);
        head = next;
    }

    thr->lock_list = NULL;
}

void idle_thread_func(void) {
    // The idle thread sleeps using wfi if the ready list is empty. Note that we
    // need to disable interrupts before checking if the thread list is empty to
    // avoid a race condition where an ISR marks a thread ready to run between
    // the call to tlempty() and the wfi instruction.

    for (;;) {
        // If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            running_thread_yield();
        
        // No runnable threads. Sleep using the wfi instruction. Note that we
        // need to disable interrupts and check the runnable thread list one
        // more time (make sure it is empty) to avoid a race condition where an
        // ISR marks a thread ready before we call the wfi instruction.

        disable_interrupts();
        if (tlempty(&ready_list))
            asm ("wfi");
        enable_interrupts();
    }
}
