// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "devimpl.h"
#include "misc.h"
#include "conf.h"
#include "intr.h"
#include "console.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "viorng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

struct viorng_serial {
    // FIXME your code goes here
    struct serial base;
    volatile struct virtio_mmio_regs *regs;
    int irqno;
    char opened;
    uint16_t qlen;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16_t avail_idx;
    uint16_t used_idx;
    uint8_t *data;
    struct condition used_not_empty;
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_serial_open(struct serial * ser);

static void viorng_serial_close(struct serial * ser);
static int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);

static void viorng_isr(int irqno, void * aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf viorng_serial_intf = {
    .blksz = 1,
    .open = &viorng_serial_open,
    .close = &viorng_serial_close,
    .recv = &viorng_serial_recv
};

struct lock virtio_locks[8]; //array of locks for virtios

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.
// viorng_attach(regs: volatile struct virtio_mmio_regs*, irqno: int)
//Inputs:
//- regs: volatile struct virtio_mmio_regs* — MMIO register block for the VirtIO RNG.
//- irqno: int — interrupt source number for the device.
//Outputs:
////- None
//Description:
//Initialize and attach a VirtIO RNG device: negotiate features, allocate/init a
//single virtqueue (desc/avail/used rings), create driver state, and register a
//serial interface for open/close/recv.
//Side Effects:
//- Writes to device status and queue MMIO registers.
//- Allocates kernel memory for device state and rings.
//- Registers the device in the global device list.
void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct viorng_serial * vrng;
    int result;
    
    assert(regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // Allocate and initialize device struct

    // FIXME your code goes here 
    vrng = kcalloc(1, sizeof(*vrng));
    vrng->regs = regs;
    vrng->irqno = irqno;
    vrng->opened = 0;
    condition_init(&vrng->used_not_empty, "viorng_used_not_empty");

    regs->queue_sel = 0;
    __sync_synchronize();
    {
        uint16_t max = regs->queue_num_max;
        if (max == 0) return;
        vrng->qlen = (max < 8) ? max : 8;
    }

    vrng->desc  = kcalloc(1, sizeof(struct virtq_desc) * vrng->qlen);
    vrng->avail = kcalloc(1, VIRTQ_AVAIL_SIZE(vrng->qlen));
    vrng->used  = kcalloc(1, VIRTQ_USED_SIZE(vrng->qlen));
    vrng->data  = kcalloc(1, VIORNG_BUFSZ);

    virtio_attach_virtq(regs, 0, vrng->qlen,
        (uint64_t)(uintptr_t)vrng->desc,
        (uint64_t)(uintptr_t)vrng->used,
        (uint64_t)(uintptr_t)vrng->avail);

    vrng->avail_idx = 0;
    vrng->used_idx  = 0;

    serial_init(&vrng->base, &viorng_serial_intf);
    register_device(VIORNG_NAME, DEV_SERIAL, &vrng->base);
    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();

    // FIXME your code goes here
}
//viorng_serial_open(struct serial* ser)
//Inputs:
//- ser: struct serial* — the serial interface base embedded in viorng_serial.
//Outputs:
//- int — 0 on success; -EBUSY if already opened.
//Description:
//Initializes and enables the VirtIO RNG device for use: installs the ISR,
//enables the virtqueue/interrupts, and marks the device as opened.
//Side Effects:
//- Registers/Enables an interrupt source for this device.
//- Writes device MMIO (enable queue / IER bits).
//- Changes driver state (opened = 1).

int viorng_serial_open(struct serial * ser) {
    // FIXME your code goes here
    struct viorng_serial *vrng = (struct viorng_serial *)ser;
    if (vrng->opened) return -EBUSY;
    enable_intr_source(vrng->irqno, VIORNG_IRQ_PRIO, viorng_isr, vrng);
    virtio_enable_virtq(vrng->regs, 0);
    vrng->opened = 1;
    return 0;
}
//viorng_serial_close(struct serial* ser)
//Inputs:
//- ser: struct serial* — the serial interface base embedded in viorng_serial.
//Outputs:
//- None
//Description:
//Closes the VirtIO RNG device: disables the ISR, resets the virtqueue, clears
//queue rings/indexes, and marks the device as closed.
//Side Effects:
//- Disables the device’s interrupt source.
//- Writes device MMIO (queue reset).
//- Zeroes avail/used rings and resets indices.
//- Changes driver state (opened = 0).

void viorng_serial_close(struct serial * ser) {
    // FIXME your code goes here
    struct viorng_serial *vrng = (struct viorng_serial *)ser;
    if (!vrng->opened) return;
    disable_intr_source(vrng->irqno);
    virtio_reset_virtq(vrng->regs, 0);
    memset(vrng->avail, 0, VIRTQ_AVAIL_SIZE(vrng->qlen));
    memset(vrng->used,  0, VIRTQ_USED_SIZE(vrng->qlen));
    vrng->avail_idx = 0;
    vrng->used_idx  = 0;
    vrng->opened = 0;
}
//viorng_serial_recv( struct serial* ser,  void* buf, unsigned int bufsz)
//Inputs:
//- ser: struct serial* — serial base embedded in viorng_serial.
//- buf: void* — destination buffer to fill with random bytes.
//- bufsz: unsigned int — maximum number of bytes to read.
//Outputs:
//- int — number of bytes written to buf (1..bufsz), or -EINVAL if not opened/invalid args.
//Description:
//Submits receive descriptors to the VirtIO RNG queue, waits for completions
//(using a condition variable), copies produced bytes into buf, and returns the
//count. Uses a per-device lock to serialize queue access.
//Side Effects:
//- Touches virtqueue rings and MMIO (notify/ack).
//- Blocks on a condition variable until data is available.
//- Modifies driver indices (avail_idx/used_idx).

int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    struct viorng_serial *vrng;
    unsigned int n = 0;
    uint16_t q, didx;
    int pie;
    
	
    if (ser == NULL || buf == NULL) return -EINVAL;
    vrng = (void*)ser - offsetof(struct viorng_serial, base);
    if (!vrng->opened) return -EINVAL;
    if (bufsz == 0) return 0;

    

    q = vrng->qlen;

    {
        int index = vrng->irqno - 1; //get index, the first virtio has irq 1
        struct lock *lk = &virtio_locks[index];
        lock_acquire(lk);

        while (n < bufsz) {
            uint32_t len = bufsz - n;
            if (len > VIORNG_BUFSZ) len = VIORNG_BUFSZ;

            didx = vrng->avail_idx % q;

            vrng->desc[didx].addr  = (uint64_t)(uintptr_t)vrng->data;
            vrng->desc[didx].len   = len;
            vrng->desc[didx].flags = VIRTQ_DESC_F_WRITE;
            vrng->desc[didx].next  = 0;

            vrng->avail->ring[didx] = didx;
            __sync_synchronize();
            vrng->avail->idx = (uint16_t)(vrng->avail_idx + 1);
            __sync_synchronize();

            virtio_notify_avail(vrng->regs, 0);

            pie = disable_interrupts();
            while (vrng->used->idx == vrng->used_idx)
                condition_wait(&vrng->used_not_empty);
            restore_interrupts(pie);

            {
                struct virtq_used_elem *ue = &vrng->used->ring[vrng->used_idx % q];
                uint32_t used_len = ue->len;
                if (used_len == 0) used_len = len;
                if (used_len > len) used_len = len;

                memcpy((uint8_t*)buf + n, vrng->data, used_len);

                vrng->used_idx++;
                vrng->avail_idx++;
                n += used_len;
            }
        }

        lock_release(lk);
    }

    return (int)n;
}
//viorng_isr(int irqno, void* aux)
//Inputs:
//- irqno: int — interrupt source number (unused aside from sanity).
//- aux: void* — opaque pointer passed at registration (viorng_serial*).
//Outputs:
//- None
//Description:
//Interrupt handler for VirtIO RNG. Reads interrupt_status, acks it, and
//signals waiters if new used entries are available.
//Side Effects:
//- Writes interrupt_ack MMIO register.
//- May wake threads waiting on the used_not_empty condition.
void viorng_isr(int irqno, void * aux) {
    // FIXME your code goes here
    struct viorng_serial *vrng = (struct viorng_serial *)aux;
    (void)irqno;
    uint32_t st = vrng->regs->interrupt_status;
    if (st) {
        vrng->regs->interrupt_ack = st;
        __sync_synchronize();
    }
    if (vrng->used->idx != vrng->used_idx)
        condition_broadcast(&vrng->used_not_empty);
}
