/*! @file uio.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌​​‌​⁠⁠‌
    @brief Uniform I/O interface
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef UIO_DEBUG
#define DEBUG
#endif

#ifdef UIO_TRACE
#define TRACE
#endif

#include "uio.h"

#include <stddef.h>  // for NULL and offsetof

#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uioimpl.h"

#include "console.h"

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char *data;
};

struct pipe {
    struct uio wio;
    struct uio rio;

    struct ringbuf buf;
    unsigned long bytecnt;

    struct lock lk;
    struct condition readable;
    struct condition writeable;

    int read_open;
    int write_open;
};

static void nulluio_close(struct uio* uio);

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz);

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen);

static void read_pipe_close(struct uio* uio);
static void write_pipe_close(struct uio* uio);
static long pipe_read(struct uio* uio, void *buf, unsigned long bufsz);
static long pipe_write(struct uio* uio, const void *buf, unsigned long bufsz);

static const struct uio_intf read_pipe_intf = {.close = &read_pipe_close, .read = &pipe_read, .write = NULL, .cntl = NULL,};
static const struct uio_intf write_pipe_intf = {.close = &write_pipe_close, .read = NULL, .write = &pipe_write, .cntl = NULL,};


// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

static long pipe_read(struct uio* uio, void *buf, unsigned long bufsz){
    struct pipe *p = (struct pipe *)((char *)uio - offsetof(struct pipe, rio));

    if (bufsz == 0)
        return 0;

    lock_acquire(&p->lk);

    while (p->bytecnt == 0 && p->write_open) {
        lock_release(&p->lk);
        condition_wait(&p->readable);
        lock_acquire(&p->lk);
    }

    if (p->bytecnt == 0 && !p->write_open){
        lock_release(&p->lk);
        return 0;
    }

    unsigned long readable_bytes = (bufsz < p->bytecnt) ? bufsz : p->bytecnt; //find min of bytes in pipe and bufsz to know how mny bytes to read

    for (unsigned long i = 0; i < readable_bytes; i++) {
        ((char *)buf)[i] = p->buf.data[p->buf.hpos];
        p->buf.hpos = (p->buf.hpos + 1) % PAGE_SIZE;
    }

    p->bytecnt -= readable_bytes;

    condition_broadcast(&p->writeable);

    lock_release(&p->lk);
    return readable_bytes;
}

static long pipe_write(struct uio* uio, const void *buf, unsigned long bufsz){
    struct pipe *p = (struct pipe *)((char *)uio - offsetof(struct pipe, wio));

    if (bufsz == 0)
        return 0;

    lock_acquire(&p->lk);

    if (p->read_open == 0){
        lock_release(&p->lk);
        return -EPIPE;
    }

    while (p->bytecnt == PAGE_SIZE && p->read_open) {
        lock_release(&p->lk);
        condition_wait(&p->writeable);
        lock_acquire(&p->lk);
    }

    if (!p->read_open){
        lock_release(&p->lk);
        return -EPIPE;
    }

    unsigned long space = PAGE_SIZE - p->bytecnt;
    unsigned long writeable_bytes = (bufsz < space) ? bufsz : space; //figure out how many bytes we can write

    for (unsigned long i = 0; i < writeable_bytes; i++) {
        p->buf.data[p->buf.tpos] = ((const char *)buf)[i];
        p->buf.tpos = (p->buf.tpos + 1) % PAGE_SIZE;
    }

    p->bytecnt += writeable_bytes;

    condition_broadcast(&p->readable);

    lock_release(&p->lk);
    return writeable_bytes;
}

static void read_pipe_close(struct uio* uio){
    struct pipe *p = (struct pipe *)((char *)uio - offsetof(struct pipe, rio));

    lock_acquire(&p->lk);
    p->read_open = 0;

    condition_broadcast(&p->writeable);

    if(p->write_open == 0){
        lock_release(&p->lk);
        free_phys_page(p->buf.data);
        kfree(p);
        return;
    }

    lock_release(&p->lk);
}

static void write_pipe_close(struct uio* uio){
    struct pipe *p = (struct pipe *)((char *)uio - offsetof(struct pipe, wio));

    lock_acquire(&p->lk);
    p->write_open = 0;

    condition_broadcast(&p->readable);

    if(p->read_open == 0){
        lock_release(&p->lk);
        free_phys_page(p->buf.data);
        kfree(p);
        return;
    }

    lock_release(&p->lk);
}

void create_pipe(struct uio **wptr, struct uio **rptr){
    struct pipe *p;
    p = kcalloc(1, sizeof(*p));
    p->buf.data = alloc_phys_page();

    p->buf.hpos = 0;
    p->buf.tpos = 0;
    p->bytecnt = 0;

    lock_init(&p->lk);
    condition_init(&p->readable, "can read pipe");
    condition_init(&p->writeable, "can write pipe");

    p->read_open = 1;
    p->write_open = 1;

    uio_init1(&p->rio, &read_pipe_intf);
    uio_init1(&p->wio, &write_pipe_intf);

    *rptr = &p->rio;
    *wptr = &p->wio;
}

void uio_close(struct uio* uio) {
    debug("uio_close: refcnt=%d, has_close=%d", uio->refcnt, (uio->intf->close != NULL));

    // Decrement reference count if it's greater than 0
    if (uio->refcnt > 0) {
        uio->refcnt--;
        debug("uio_close: decremented refcnt to %d", uio->refcnt);
    }

    // Only call the actual close method when refcnt reaches 0
    if (uio->refcnt == 0 && uio->intf->close != NULL) {
        debug("uio_close: calling close method");
        uio->intf->close(uio);
    } else if (uio->refcnt > 0) {
        debug("uio_close: NOT calling close (refcnt=%d still has references)", uio->refcnt);
    }
}

long uio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    if (uio->intf->read != NULL) {
        if (0 <= (long)bufsz)
            return uio->intf->read(uio, buf, bufsz);
        else{
            return -EINVAL;
        }
    } else{
        return -ENOTSUP;
    }
}

long uio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    if (uio->intf->write != NULL) {
        if (0 <= (long)buflen)
            return uio->intf->write(uio, buf, buflen);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

int uio_cntl(struct uio* uio, int op, void* arg) {
    if (uio->intf->cntl != NULL)
        return uio->intf->cntl(uio, op, arg);
    else
        return -ENOTSUP;
}

unsigned long uio_refcnt(const struct uio* uio) {
    assert(uio != NULL);
    return uio->refcnt;
}

int uio_addref(struct uio* uio) { return ++uio->refcnt; }

struct uio* create_null_uio(void) {
    static const struct uio_intf nulluio_intf = {
        .close = &nulluio_close, .read = &nulluio_read, .write = &nulluio_write};

    static struct uio nulluio = {.intf = &nulluio_intf, .refcnt = 0};

    return &nulluio;
}

static void nulluio_close(struct uio* uio) {
    // ...
}

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // ...
    return -ENOTSUP;
}

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    // ...
    return -ENOTSUP;
}
