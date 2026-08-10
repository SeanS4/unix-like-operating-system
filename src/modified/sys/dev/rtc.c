// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "rtc.h"
#include "conf.h"
#include "misc.h"
#include "devimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low;  // read first, latches time_high
    uint32_t time_high; //
};

struct rtc_device {
    struct serial base; // must be first
    volatile struct rtc_regs * regs;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct serial * ser);
static void rtc_close(struct serial * ser);
static int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

static const struct serial_intf rtc_serial_intf = {
    .blksz = 8,
    .open = &rtc_open,
    .close = &rtc_close,
    .recv = &rtc_recv
};

// EXPORTED FUNCTION DEFINITIONS
// 
//void rtc_attach(void *mmio_base)
//Inputs:
//void *mmio_base - MMIO base address of the RTC hardware registers.
//Outputs:
//None
//Description:
//Allocates and initializes an RTC device structure, initializes its serial interface,
//maps the provided MMIO base to the device’s register pointer, and registers the
//device under the name "rtc".
//Side Effects:
//Allocates kernel memory, touches device MMIO registers, and updates global device
//registry (device becomes discoverable via find_serial("rtc", 0)).
void rtc_attach(void * mmio_base) {
    // FIXME your code goes here
    struct rtc_device * rtc = kcalloc(1, sizeof(*rtc));
    serial_init(&rtc->base, &rtc_serial_intf);
    rtc->regs = (volatile struct rtc_regs *) mmio_base;
    register_device("rtc", DEV_SERIAL, &rtc->base);
}

int rtc_open(struct serial * ser) {
    trace("%s()", __func__);
    return 0;
}

void rtc_close(struct serial * ser) {
    trace("%s()", __func__);
}
//int rtc_recv(struct serial *ser, void *buf, unsigned int bufsz)
//Inputs:
//struct serial *ser - Pointer to the RTC device’s serial interface.
//void *buf - Destination buffer for timestamp bytes.
//unsigned int bufsz - Number of bytes to read into buf.
//Outputs:
//int - Number of bytes written to buf on success, or -EINVAL on invalid args.
//Description:
//Reads the current 64-bit real-time clock value repeatedly and copies bytes
//into buf until exactly bufsz bytes are produced. Works for any bufsz.
//Returns the exact count written.
//Side Effects:
//None
int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    if (!ser || !buf) return -EINVAL;
    if (bufsz == 0)   return 0;

    struct rtc_device *rtc = (void *)((char *)ser - offsetof(struct rtc_device, base));
    uint8_t *p = buf;
    unsigned int n = 0;

    while (n < bufsz) {
        uint64_t t = read_real_time(rtc->regs);
        unsigned int chunk = bufsz - n;
        if (chunk > 8) chunk = 8;
        memcpy(p + n, &t, chunk);
        n += chunk;
    }

    return (int)n;

}
//uint64_t read_real_time(volatile struct rtc_regs *regs)
//Inputs:
//volatile struct rtc_regs *regs - Pointer to the RTC MMIO register block.
//Outputs:
//uint64_t - 64-bit current time value assembled from high/low registers.
//Description:
//Reads the RTC’s 32-bit low and 32-bit high time registers and combines them
//into a single 64-bit timestamp: (time_high << 32) | time_low.
//Side Effects:
//None
uint64_t read_real_time(volatile struct rtc_regs * regs) {
    // FIXME your code goes here
    uint32_t time_low = regs->time_low;
    uint64_t time_high = regs->time_high;
    uint64_t time_now = time_low + (time_high << 32);
    return time_now;

}
