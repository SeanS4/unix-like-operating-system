/*! @file syscall.c
    @brief system call handlers
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#define EXEC_MAXARGS 8   // match shell

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "memory.h"
#include "misc.h"
#include "process.h"
#include "scnum.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "uio.h"

// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame *tfr);  // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame *tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char **argv);
static int sysfork(const struct trap_frame *tfr);  
static int syswait(int tid);
static int sysprint(const char *msg);
static int sysusleep(unsigned long us);

static int sysfsdelete(const char *path);
static int sysfscreate(const char *path);

static int sysopen(int fd, const char *path);
static int sysclose(int fd);
static long sysread(int fd, void *buf, size_t bufsz);
static long syswrite(int fd, const void *buf, size_t len);
static int sysfcntl(int fd, int cmd, void *arg);
static int syspipe(int *wfdptr, int *rfdptr);        
static int sysuiodup(int oldfd, int newfd);

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Initiates syscall present in trap frame struct and stores the return address into the sepc
 * @details sepc will be used to return back to program execution after interrupt is handled and
 * sret is called
 * @param tfr pointer to trap frame struct
 * @return void
 */

void handle_syscall(struct trap_frame *tfr) {
    tfr->sepc += 4; // continue from next instruction
    // Execute syscall
    tfr->a0 = syscall(tfr);
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Calls specified syscall and passes arguments
 * @details Function uses register a7 to determine syscall number and arguments are passed in from
 * a0-a5 depending on the function
 * @param tfr pointer to trap frame struct
 * @return result of syscall
 */

int64_t syscall(const struct trap_frame *tfr) { 
    switch (tfr->a7) {

    case SYSCALL_EXIT:
        return sysexit();
        break;
        
    case SYSCALL_EXEC:
        return sysexec((int)tfr->a0, (int)tfr->a1, (char **)tfr->a2);
        break;

    case SYSCALL_FORK:
        return sysfork(tfr);
        break;

    case SYSCALL_WAIT:
        return syswait((int)tfr->a0);
        break;

    case SYSCALL_PRINT:
        return sysprint((const char*)tfr->a0);
        break;

    case SYSCALL_USLEEP:
        return sysusleep((unsigned long)tfr->a0);
        break;

    case SYSCALL_FSCREATE:
        return sysfscreate((const char *)tfr->a0);
        break;

    case SYSCALL_FSDELETE:
        return sysfsdelete((const char *)tfr->a0);
        break;

    case SYSCALL_OPEN:
        return sysopen((int)tfr->a0, (const char *)tfr->a1);
        break;

    case SYSCALL_CLOSE:
        return sysclose((int)tfr->a0);
        break;

    case SYSCALL_READ:
        return sysread((int)tfr->a0, (void *)tfr->a1, (size_t)tfr->a2);
        break;

    case SYSCALL_WRITE:
        return syswrite((int)tfr->a0, (const void *)tfr->a1, (size_t)tfr->a2);
        break;

    case SYSCALL_FCNTL:
        return sysfcntl((int)tfr->a0, (int)tfr->a1, (void *)tfr->a2);
        break;

    case SYSCALL_PIPE:
        return syspipe((int *)tfr->a0, (int *)tfr->a1);
        break;  

    case SYSCALL_UIODUP:
        return sysuiodup((int)tfr->a0, (int)tfr->a1);
        break;

    default:
        return -EINVAL;
    }
}

/**
 * @brief Calls process exit
 * @return void
 */

int sysexit(void) { 
    process_exit();
    return 0;
}

/**
 * @brief Executes new process given a executable and arguments
 * @details Valid fd checks, get current process struct, close fd being executed, finally calls
 * process_exec with arguments and executable io "file"
 * @param fd file descripter idx
 * @param argc number of arguments in argv
 * @param argv array of arguments for multiple args
 * @return result of process_exec, else -EBADFD on invalid file descriptors
 */

    int sysexec(int fd, int argc, char **argv) {
    struct process *proc = running_thread_process();
    struct uio *file;
    char *kargv[EXEC_MAXARGS + 1];
    int rc;

    if (fd < 0 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    file = proc->uiotab[fd];
    if (file == NULL)
        return -EBADFD;

    if (argc < 0 || argc > EXEC_MAXARGS)
        return -EINVAL;

    for (int i = 0; i < EXEC_MAXARGS + 1; i++)
        kargv[i] = NULL;

    if (argc == 0) {
        rc = process_exec(file, 0, NULL);
        if (rc == 0)
            proc->uiotab[fd] = NULL;
        return rc;
    }

    rc = validate_vptr(argv, (argc + 1) * sizeof(char *), PTE_R | PTE_U);
    if (rc < 0)
        return rc;

    if (argv[argc] != NULL)
        return -EINVAL;

    for (int i = 0; i < argc; i++) {
        if (argv[i] == NULL) {
            rc = -EINVAL;
            goto fail;
        }

        rc = validate_vstr(argv[i], PTE_R | PTE_U);
        if (rc < 0)
            goto fail;

        size_t len = strlen(argv[i]) + 1;
        kargv[i] = kmalloc(len);
        if (kargv[i] == NULL) {
            rc = -ENOMEM;
            goto fail;
        }

        memcpy(kargv[i], argv[i], len);
    }

    kargv[argc] = NULL;

    rc = process_exec(file, argc, kargv);
    if (rc == 0) {
        proc->uiotab[fd] = NULL;
        return 0;
    }

fail:
    for (int i = 0; i < argc; i++) {
        if (kargv[i] != NULL)
            kfree(kargv[i]);
    }
    return rc;
}


// /**
//  * @brief Forks a new child process using process_fork
//  * @param tfr pointer to the trap frame
//  * @return result of process_fork
//  */

int sysfork(const struct trap_frame *tfr) { 
    int rc = process_fork(tfr);
    return rc;
}           

/**
 * @brief Sleeps till a specified child process completes
 * @details Calls thread_join with the thread id the process wishes to wait for
 * @param tid thread_id
 * @return result of thread_join else invalid on invalid thread id
 */

int syswait(int tid) {    

    trace("%s(%d)", __func__, tid);
 
    if (0 <= tid)
        return thread_join(tid);
    else
        return -EINVAL;
}  

/**
 * @brief Prints to console via kprintf
 * @details Validates that msg string is valid via validate_vstr and pages are mapped, calls kprintf
 * on current running process
 * @param msg string msg in userspace
 * @return 0 on sucess else error from validate_vstr
 */

int sysprint(const char *msg) { 
    int rc = validate_vstr(msg, PTE_R | PTE_U);
    if (rc < 0)
        return rc;

    kprintf("%s", msg);
    return 0;
}

/**
 * @brief Sleeps process till specificed amount of time has passed
 * @details Creates alarm struct, inits struct with name usleep, which sets the current time via the
 * rd_time() function, taking values from the csr, makes frequency calcuation to determine us has
 * passed before waking process
 * @param us time in us for process to sleep
 * @return 0
 */

int sysusleep(unsigned long us) { 
    struct alarm al;
    alarm_init(&al, "usleep");
    alarm_sleep_us(&al, us);
    return 0; 
}

/**
 * @brief Creates a new file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * create_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfscreate(const char *path) {
    int rc = validate_vstr(path, PTE_R | PTE_U);
    if (rc < 0)
        return rc;

    char kpath[128];
    strncpy(kpath, path, sizeof(kpath));
    kpath[sizeof(kpath) - 1] = '\0';

    char *mpname;
    char *flname;

    rc = parse_path(kpath, &mpname, &flname);
    if (rc < 0)
        return rc;

    return create_file(mpname, flname);
}


/**
 * @brief Deletes a file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * delete_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfsdelete(const char *path) {
    int rc = validate_vstr(path, PTE_R | PTE_U);
    if (rc < 0)
        return rc;

    char kpath[128];
    strncpy(kpath, path, sizeof(kpath));
    kpath[sizeof(kpath) - 1] = '\0';

    char *mpname;
    char *flname;

    rc = parse_path(kpath, &mpname, &flname);
    if (rc < 0)
        return rc;

    return delete_file(mpname, flname);
}


/**
 * @brief Opens a file or device of specified fd for given process
 * @details gets current process, allocates file descriptor (if fd = -1) or uses valid file
 * descriptor given, validates and parses user provided path, calls open_file
 * @param fd file descriptor number
 * @param path User provided path string
 * @return fd number if sucessful else return error that occured -EMFILE or -EBADFD
 */

int sysopen(int fd, const char *path) { 
    struct process *proc = running_thread_process();

    int rc = validate_vstr(path, PTE_R | PTE_U);
    if (rc < 0)
        return rc;

    if (fd < -1 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    if(fd == -1){
        for(int i = 0; i < PROCESS_UIOMAX; i++){
            if(proc->uiotab[i] == NULL){
                fd = i;
                break;
            }
        }
    }

    if(fd == -1)
        return -EMFILE;

    if(proc->uiotab[fd] != NULL)
        return -EBADFD;

    char *mpname;
    char *flname;
    struct uio *u;

    char kpath[128];
    strncpy(kpath, path, sizeof(kpath));
    kpath[sizeof(kpath) - 1] = '\0';

    rc = parse_path(kpath, &mpname, &flname);
    if(rc < 0)
        return rc;

    rc = open_file(mpname, flname, &u);
    if(rc < 0)
        return rc;

    proc->uiotab[fd] = u;
    return fd;
}

/**
 * @brief Closes file or device of specified fd for given process
 * @details gets current process, calls close function of the io, deallocates the file descriptor
 * @param fd file descriptor
 * @return 0 on success, error on invalid file descriptor or empty file descriptor
 */

int sysclose(int fd) { 
    struct process *proc = running_thread_process();

    if (fd < 0 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    if (proc->uiotab[fd] == NULL)
        return -EBADFD;

    struct uio *u = proc->uiotab[fd];

    uio_close(u);

    proc->uiotab[fd] = NULL;
    return 0;
}

/**
 * @brief Calls read function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call ioread with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param bufsz number of bytes to be read
 * @return number of bytes read
 */

long sysread(int fd, void *buf, size_t bufsz) { 
    struct process *proc = running_thread_process();
    struct uio *file;

    if (fd < 0 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    if (proc->uiotab[fd] == NULL)
        return -EBADFD;

    file = proc->uiotab[fd];

    int rc = validate_vptr(buf, bufsz, PTE_W | PTE_U);
    if(rc < 0)
        return rc;

    long bytecnt = uio_read(file, buf, bufsz);
    return bytecnt;
}

/**
 * @brief Calls write function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call iowrite with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param len number of bytes to be written
 * @return number of bytes written
 */

long syswrite(int fd, const void *buf, size_t len) {
    struct process *proc = running_thread_process();
    struct uio *file;

    if (fd < 0 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    if (proc->uiotab[fd] == NULL)
        return -EBADFD;

    file = proc->uiotab[fd];

    int rc = validate_vptr(buf, len, PTE_R | PTE_U);
    if(rc < 0)
        return rc;

    long bytecnt = uio_write(file, buf, len);

    return bytecnt;
}

/**
 * @brief Calls device input output commands for a given device instance
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * ensure that fcntl type exists, validate argument pointer, issue fcntl
 * @param fd file descriptor number
 * @param cmd selection of fcntl
 * @param arg pointer to arguments
 * @return number of bytes written
 */

int sysfcntl(int fd, int cmd, void *arg) { 
    struct process *proc = running_thread_process();
    struct uio *file;

    if (fd < 0 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    if (proc->uiotab[fd] == NULL)
        return -EBADFD;

    file = proc->uiotab[fd];

    if (cmd == FCNTL_GETEND || cmd == FCNTL_GETPOS){
        int rc = validate_vptr(arg, sizeof(unsigned long long), PTE_W | PTE_U);
        if(rc < 0)
            return rc;
    }
    else if (cmd == FCNTL_SETEND || cmd == FCNTL_SETPOS){
        int rc = validate_vptr(arg, sizeof(unsigned long long), PTE_R | PTE_U);
        if(rc < 0)
            return rc;
    }
    else if (cmd == FCNTL_MMAP){
        int rc = validate_vptr(arg, sizeof(void *), PTE_W | PTE_U);
        if(rc < 0)
            return rc;
    }
    else{
        return -EINVAL;
    }

    return uio_cntl(file, cmd, arg);
}

// /**
//  * @brief Creates a pipe for the current process
//  * @details The function retrieves the current process. If either the write or read descriptor
//  * pointer stores a negative value, an unused descriptor is assigned. If both file descriptors are
//  * unused and valid, the function connects them via create_pipe function.
//  * @param wfdptr pointer to write file descriptor
//  * @param rfdptr pointer to read file descriptor
//  * @return 0 on success. Else, negative error code on invalid file descriptor, or if a file
//  * descriptor is already in use, or if no descriptors are found available.
//  */
int syspipe(int *wfdptr, int *rfdptr) {
    int rc = validate_vptr(wfdptr, sizeof(int), PTE_W | PTE_U);
        if(rc < 0)
            return rc;

    rc = validate_vptr(rfdptr, sizeof(int), PTE_W | PTE_U);
        if(rc < 0)
            return rc;

    struct process *proc = running_thread_process();

    int wfd = *wfdptr;
    int rfd = *rfdptr;

    struct uio *wio;
    struct uio *rio;

    if(wfd < 0){
        for(int i = 0; i < PROCESS_UIOMAX; i++){
            if(proc->uiotab[i] == NULL && i != rfd){
                wfd = i;
                break;
            }
        }
    }

    if (wfd < 0 || wfd >= PROCESS_UIOMAX)
        return -EBADFD;

    if (proc->uiotab[wfd] != NULL)
        return -EBUSY;

    if(rfd < 0){
        for(int i = 0; i < PROCESS_UIOMAX; i++){
            if(proc->uiotab[i] == NULL && i != wfd){
                rfd = i;
                break;
            }
        }
    }

    if (rfd < 0 || rfd >= PROCESS_UIOMAX)
        return -EBADFD;

    if (proc->uiotab[rfd] != NULL)
        return -EBUSY;

    if (wfd == rfd)
        return -EBADFD;

    create_pipe(&wio, &rio);
    
    proc->uiotab[wfd] = wio;
    proc->uiotab[rfd] = rio;

    *wfdptr = wfd;
    *rfdptr = rfd;

    return 0;
}    

/**
 * @brief Duplicates a file description
 * @details Allocates a new file descriptor that refers to the same open _uio_ as the descriptor
 * _oldfd_. Increments the _refcnt_ if successful.
 * @param oldfd old file descriptor number
 * @param newfd new file descriptor number
 * @return fd number if sucessful else return error on invalid file descriptor or empty file
 * descriptor
 */

int sysuiodup(int oldfd, int newfd) {
    struct process *proc = running_thread_process();
    struct uio *oldfile;

    if (oldfd < 0 || oldfd >= PROCESS_UIOMAX)
        return -EBADFD;

    oldfile = proc->uiotab[oldfd];

    if (oldfile == NULL)
        return -EBADFD;

    if(newfd < 0){
        for(int i = 0; i < PROCESS_UIOMAX; i++){
            if(proc->uiotab[i] == NULL){
                newfd = i;
                break;
            }
        }
    }

    if (newfd < 0)
        return -EMFILE;

    if (newfd >= PROCESS_UIOMAX)
        return -EBADFD;

    if (proc->uiotab[newfd] != NULL)
        return -EBADFD;

    proc->uiotab[newfd] = oldfile;
    uio_addref(oldfile);
    return newfd;
}
