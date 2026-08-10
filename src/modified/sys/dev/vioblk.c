/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌​​‌​⁠⁠‌
    @brief VirtIO block device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "devimpl.h"
#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include <limits.h>

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "heap.h"
#include "intr.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"  // FCNTL
#include "virtio.h"

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//

struct virtio_blk_req_header{
    uint32_t type;      //explains the type of request to vioblk
    uint32_t reserved;  //memory allignment padding
    uint64_t sector;    //defines the offset of the read/write location (sector * 512)     
};

//struct defining vioblk
struct vioblk_storage {
    struct storage base;    //holds intf and capacity
    volatile struct virtio_mmio_regs *regs;
    int irqno;
    char opened;

    //virtq
    uint16_t qlen;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16_t prev_used_idx;

    uint32_t blksz; //block size
    struct storage_intf intf;

    //virtio_blk_req format
    struct virtio_blk_req_header *header;  //notifies blk device of I/O request type and location
    uint8_t *data;     //pointer to data buffer for read/write
    uint8_t *status;     //pointer to status of last I/O operation

    struct lock virtq_lock;
    struct condition used_not_empty;
};

//request type codes
#define VIRTIO_BLK_T_IN           0  //read from vioblk
#define VIRTIO_BLK_T_OUT          1  //write to vioblk
#define VIRTIO_BLK_T_FLUSH        4 
#define VIRTIO_BLK_T_GET_ID       8  //fetches device ID
#define VIRTIO_BLK_T_GET_LIFETIME 10 
#define VIRTIO_BLK_T_DISCARD      11 
#define VIRTIO_BLK_T_WRITE_ZEROES 13 
#define VIRTIO_BLK_T_SECURE_ERASE   14

//status byte codes
#define VIRTIO_BLK_S_OK        0  //success
#define VIRTIO_BLK_S_IOERR     1  //error
#define VIRTIO_BLK_S_UNSUPP    2  //unsuported request

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14

#define SECTOR_SIZE 512
// INTERNAL FUNCTION DECLARATIONS
//

/**
 * @brief Sets the virtq avail and virtq used queues such that they are available for use. (Hint,
 * read virtio.h) Enables the interupt line for the virtio device and sets necessary flags in vioblk
 * device.
 * @param sto Storage IO struct for the storage device
 * @return Return 0 on success or negative error code if error. If the given sto is already opened,
 * then return -EBUSY.
 */
static int vioblk_storage_open(struct storage* sto);

/**
 * @brief Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device. If
 * the given sto is not opened, this function does nothing.
 * @param sto Storage IO struct for the storage device
 * @return None
 */
static void vioblk_storage_close(struct storage* sto);

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf. Achieves this by
 * repeatedly setting the appropriate registers to request a block from the disk, waiting until the
 * data has been populated in block buffer cache, and then writes that data out to buf. Thread
 * sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the read within the VirtIO device
 * @param buf A pointer to the buffer to fill with the read data
 * @param bytecnt The number of bytes to read from the VirtIO device into the buffer
 * @return The number of bytes read from the device, or negative error code if error
 */
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Writes bytecnt number of bytes from the parameter buf to the disk. The size of the virtio
 * device should not change. You should only overwrite existing data. Write should also not create
 * any new files. Achieves this by filling up the block buffer cache and then setting the
 * appropriate registers to request the disk write the contents of the cache to the specified block
 * location. Thread sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the write within the VirtIO device
 * @param buf A pointer to the buffer with the data to write
 * @param bytecnt The number of bytes to write to the VirtIO device from the buffer
 * @return The number of bytes written to the device, or negative error code if error
 */
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions on the VirtIO block device.
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage IO struct for the storage device
 * @param op Operation to execute. vioblk should support FCNTL_GETEND.
 * @param arg Argument specific to the operation being performed
 * @return Status code on the operation performed
 */
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg);

/**
 * @brief The interrupt handler for the VirtIO device. When an interrupt occurs, the system will
 * call this function.
 * @param irqno The interrupt request number for the VirtIO device
 * @param aux A generic pointer for auxiliary data.
 * @return None
 */
static void vioblk_isr(int irqno, void* aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.
/**
 * @brief Initializes virtio block device with the necessary IO operation functions and sets the
 * required feature bits.
 * @param regs Memory mapped register of Virtio
 * @param irqno Interrupt request number of the device
 * @return None
 */
void vioblk_attach(volatile struct virtio_mmio_regs* regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_storage* vbd;
    unsigned int blksz;
    int result;

    trace("%s(regs=%p,irqno=%d)", __func__, regs, irqno);

    assert(regs->device_id == VIRTIO_ID_BLOCK);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize();  // fence o,io

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and            
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // FIXME
    vbd = kcalloc(1, sizeof(*vbd));
    vbd->regs = regs;
    vbd->irqno = irqno; 
    vbd->opened = 0;
    vbd->blksz = blksz;
    vbd->prev_used_idx = 0;

    lock_init(&vbd->virtq_lock);
    condition_init(&vbd->used_not_empty, "vioblk_used_not_empty");

     regs->queue_sel = 0;
    __sync_synchronize();
    {
        uint16_t max = regs->queue_num_max;
        if (max < 3) return;  //return if there aren't enough descriptors for one blk_req
        vbd->qlen = (max < 8) ? max : 8;
    }

    vbd->intf.blksz = blksz;
    vbd->intf.open = &vioblk_storage_open;
    vbd->intf.close = &vioblk_storage_close;
    vbd->intf.fetch = &vioblk_storage_fetch;
    vbd->intf.store = &vioblk_storage_store;
    vbd->intf.cntl = &vioblk_storage_cntl;

    vbd->desc  = kcalloc(1, sizeof(struct virtq_desc) * vbd->qlen);
    vbd->avail = kcalloc(1, VIRTQ_AVAIL_SIZE(vbd->qlen));
    vbd->used  = kcalloc(1, VIRTQ_USED_SIZE(vbd->qlen));

    vbd->header = kcalloc(1, sizeof(struct virtio_blk_req_header));
    vbd->data = kcalloc(1, blksz * 2); //allows driver to interact with 2 sectors at a time to deal with potential overlap between sectors
    vbd->status = kcalloc(1, sizeof(uint8_t));

    virtio_attach_virtq(regs, 0, vbd->qlen,
        (uint64_t)(uintptr_t)vbd->desc,
        (uint64_t)(uintptr_t)vbd->used,
        (uint64_t)(uintptr_t)vbd->avail);

    storage_init(&vbd->base, &vbd->intf, regs->config.blk.capacity * SECTOR_SIZE); //capacity is expressed in terms of 512 bytes per sector
    register_device(VIOBLK_NAME, DEV_STORAGE, &vbd->base);
    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();
}

static int vioblk_storage_open(struct storage* sto) {
    // FIXME
    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;  //defines pointer to vioblk struct 
    if (vbd->opened) return -EBUSY;
    lock_acquire(&vbd->virtq_lock);

    virtio_attach_virtq(vbd->regs, 0, vbd->qlen,
        (uint64_t)(uintptr_t)vbd->desc,
        (uint64_t)(uintptr_t)vbd->used,
        (uint64_t)(uintptr_t)vbd->avail);

    enable_intr_source(vbd->irqno, VIOBLK_INTR_PRIO, vioblk_isr, vbd);
    virtio_enable_virtq(vbd->regs, 0);
    vbd->opened = 1;

    lock_release(&vbd->virtq_lock);
    return 0;
}

static void vioblk_storage_close(struct storage* sto) {
    // FIXME
    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;  //defines pointer to vioblk struct 
    if (!vbd->opened) return;
    lock_acquire(&vbd->virtq_lock);

    disable_intr_source(vbd->irqno);
    virtio_reset_virtq(vbd->regs, 0);
    // memset(vbd->avail, 0, VIRTQ_AVAIL_SIZE(vbd->qlen));     this was meant to zero the virtq indicies and reset the virtq memory
    // memset(vbd->used, 0, VIRTQ_USED_SIZE(vbd->qlen));       however, there is an issue specifically with zeroing avail->idx and memeset(avail)
    // vbd->avail->idx = 0;                                    the device must remember the old avail in the hardware or something
    // vbd->used->idx  = 0;                                    the code appears functional without these resets but this should be 
    // vbd->prev_used_idx = 0;                                 revisited if vioblk doesn't pass all tests.
    vbd->opened = 0;                                         
    
    lock_release(&vbd->virtq_lock);
}

static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt) {
    // FIXME
    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;  //defines pointer to vioblk struct 
    if (vbd->opened == 0 || bytecnt == 0) return -EINVAL;
    
    unsigned long bytesleft = bytecnt;
    uint8_t *out = (uint8_t *)buf;

    lock_acquire(&vbd->virtq_lock);

    if(bytecnt == 0){
        lock_release(&vbd->virtq_lock);
        return 0;
    }

    while(bytesleft >= vbd->blksz){          //iterates until there aren't enough bytes requested left to fill a blksz
        unsigned int offset = pos % SECTOR_SIZE;

        uint64_t sec = (pos - offset) / SECTOR_SIZE;
        unsigned int sectors_needed = vbd->blksz/SECTOR_SIZE;
        if(offset != 0)
            sectors_needed++;

        uint64_t capacity_sectors = vbd->base.capacity/SECTOR_SIZE;

        if(sec + sectors_needed > capacity_sectors)  //break if blksz bytes will overflow past capacity
            break;

        // lock_acquire(&vbd->virtq_lock);

        vbd->header->type = VIRTIO_BLK_T_IN;   //define blk_req_header
        vbd->header->reserved = 0;
        vbd->header->sector = sec;

        vbd->desc[0].addr = (uint64_t)(uintptr_t)vbd->header;       
        vbd->desc[0].len = sizeof(*vbd->header);
        vbd->desc[0].flags = VIRTQ_DESC_F_NEXT;
        vbd->desc[0].next = 1;

        *vbd->status = 0xFF; //mark request as pending

        vbd->desc[1].addr = (uint64_t)(uintptr_t)vbd->data;
        vbd->desc[1].len = (uint32_t)(SECTOR_SIZE * sectors_needed);
        vbd->desc[1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;        
        vbd->desc[1].next = 2;

        vbd->desc[2].addr = (uint64_t)(uintptr_t)vbd->status;
        vbd->desc[2].len = 1;
        vbd->desc[2].flags = VIRTQ_DESC_F_WRITE;
        vbd->desc[2].next = 0;

        vbd->avail->ring[vbd->avail->idx % vbd->qlen] = 0;   //adds desc chain to avail ring 
        __sync_synchronize();
        vbd->avail->idx ++;
        vbd->prev_used_idx = vbd->used->idx;

        
        virtio_notify_avail(vbd->regs, 0);

        int pie = disable_interrupts();
        while(vbd->used->idx == vbd->prev_used_idx)
            condition_wait(&vbd->used_not_empty);
        restore_interrupts(pie);

        if(*vbd->status ==  VIRTIO_BLK_S_UNSUPP){
            lock_release(&vbd->virtq_lock);
            return -ENOTSUP; //operation not supported
        }

        if(*vbd->status ==  VIRTIO_BLK_S_IOERR){
            lock_release(&vbd->virtq_lock);
            return -EIO; // IO error 
        }

        memcpy(out, ((uint8_t*)vbd->data) + offset, vbd->blksz);

        out += vbd->blksz;
        pos += vbd->blksz;
        bytesleft -= vbd->blksz;
    }
    lock_release(&vbd->virtq_lock);

    return bytecnt - bytesleft;  //return the total amount of bytes copied
}

static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt) {
    // FIXME
    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;  //defines pointer to vioblk struct 
    if (vbd->opened == 0 || bytecnt == 0) return -EINVAL;
    
    unsigned long bytesleft = bytecnt;
    const uint8_t *in = (const uint8_t *)buf;

    lock_acquire(&vbd->virtq_lock);

    while(bytesleft >= vbd->blksz){          //iterates until there aren't enough bytes requested left to fill a blksz
        unsigned int offset = pos % SECTOR_SIZE;

        uint64_t sec = (pos - offset) / SECTOR_SIZE;
        unsigned int sectors_needed = vbd->blksz/SECTOR_SIZE;
        if(offset != 0)
            sectors_needed++;

        uint64_t capacity_sectors = vbd->base.capacity/SECTOR_SIZE;

        if(sec + sectors_needed > capacity_sectors)  //break if blksz bytes will overflow past capacity
            break;

        vbd->header->type = VIRTIO_BLK_T_OUT;   //define blk_req_header 
        vbd->header->reserved = 0;
        vbd->header->sector = sec;

        vbd->desc[0].addr = (uint64_t)(uintptr_t)vbd->header;       
        vbd->desc[0].len = sizeof(*vbd->header);
        vbd->desc[0].flags = VIRTQ_DESC_F_NEXT;
        vbd->desc[0].next = 1;

        *vbd->status = 0xFF; //mark request as pending

        // copy data from input buffer into vbd->data before writing to disk
        memcpy(((uint8_t*)vbd->data) + offset, in, vbd->blksz);

        vbd->desc[1].addr = (uint64_t)(uintptr_t)vbd->data;
        vbd->desc[1].len = (uint32_t)(SECTOR_SIZE * sectors_needed);
        vbd->desc[1].flags = VIRTQ_DESC_F_NEXT;   // write TO disk, so device reads FROM our buffer
        vbd->desc[1].next = 2;

        vbd->desc[2].addr = (uint64_t)(uintptr_t)vbd->status;
        vbd->desc[2].len = 1;
        vbd->desc[2].flags = VIRTQ_DESC_F_WRITE;
        vbd->desc[2].next = 0;

        vbd->avail->ring[vbd->avail->idx % vbd->qlen] = 0;   //adds desc chain to avail ring 
        __sync_synchronize();
        vbd->avail->idx ++;
        vbd->prev_used_idx = vbd->used->idx;
        
        
        virtio_notify_avail(vbd->regs, 0);

        int pie = disable_interrupts();
        while(vbd->used->idx == vbd->prev_used_idx)
            condition_wait(&vbd->used_not_empty);
        restore_interrupts(pie);

        if(*vbd->status ==  VIRTIO_BLK_S_UNSUPP){
            lock_release(&vbd->virtq_lock);
            return -ENOTSUP; //operation not supported
        }

        if(*vbd->status ==  VIRTIO_BLK_S_IOERR){
            lock_release(&vbd->virtq_lock);
            return -EIO; // IO error 
        }

        in += vbd->blksz;
        pos += vbd->blksz;
        bytesleft -= vbd->blksz;
    }
    lock_release(&vbd->virtq_lock);
    return bytecnt - bytesleft;  //return the total amount of bytes written
}

static int vioblk_storage_cntl(struct storage* sto, int op, void* arg) {
    // FIXME
    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;  //defines pointer to vioblk struct 
    if (vbd->opened == 0) return -EINVAL;
    
    if(op == FCNTL_GETEND){
        if(arg == NULL) return -EINVAL;
        *(unsigned long long *)arg = vbd->base.capacity; //capacity in bytes
        return 0;
    }
    return -ENOTSUP;
}

static void vioblk_isr(int irqno, void* aux) {
    // FIXME
    struct vioblk_storage *vbd = (struct vioblk_storage *)aux;
    (void)irqno;
    uint32_t st = vbd->regs->interrupt_status;

    if (st) {
        vbd->regs->interrupt_ack = st;
        __sync_synchronize();
        condition_broadcast(&vbd->used_not_empty);
    }

    if(vbd->used->idx != vbd->prev_used_idx)
        condition_broadcast(&vbd->used_not_empty);
}