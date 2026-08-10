// uart.c -  NS8250-compatible serial port
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "misc.h"
#include "uart.h"
#include "devimpl.h"
#include "intr.h"
#include "heap.h"
#include "thread.h"
#include "console.h"

#include "error.h"

#include <stdint.h>

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_DEVNAME
#define UART_DEVNAME "uart"
#endif


// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

// Simple fixed-size ring buffer

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

// UART device structure

struct uart_serial {
    struct serial base;
    volatile struct uart_regs * regs;
    int irqno;
    char opened;

    unsigned long rxovrcnt; ///< number of times OE was set
    
    struct condition rxbnotempty; ///< signalled when rxbuf becomes not empty
    struct condition txbnotfull;  ///< signalled when txbuf becomes not full

    struct ringbuf rxbuf;
    struct ringbuf txbuf;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_serial_open(struct serial * ser);
static void uart_serial_close(struct serial * ser);
static int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);
static int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz);

static void uart_isr(int srcno, void * aux);

// Ring buffer (struct rbuf) functions

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf uart_serial_intf = {
    .blksz = 1,
    .open = &uart_serial_open,
    .close = &uart_serial_close,
    .recv = &uart_serial_recv,
    .send = &uart_serial_send
};

struct lock uart_locks[8]; //locks for each UART

// EXPORTED FUNCTION DEFINITIONS
// 


void attach_uart(void * mmio_base, int irqno) {
    struct uart_serial * uart;

    trace("%s(%p,%d)", __func__, mmio_base, irqno);
    
    // UART0 is used for the console and should not be attached as a normal
    // device. It should already be initialized by console_init(). We still
    // register the device (to reserve the name uart0), but pass a NULL device
    // pointer, so that find_serial("uart", 0) returns NULL.

    if (mmio_base == (void*)UART0_MMIO_BASE) {
        register_device(UART_DEVNAME, DEV_SERIAL, NULL);
        return;
    }
    
    uart = kcalloc(1, sizeof(struct uart_serial));

    uart->regs = mmio_base;
    uart->irqno = irqno;
    uart->opened = 0;

    // Initialize condition variables. The ISR is registered when our interrupt
    // source is enabled in uart_serial_open().

    condition_init(&uart->rxbnotempty, "uart.rxnotempty");
    condition_init(&uart->txbnotfull, "uart.txnotfull");


    // Initialize hardware

    uart->regs->ier = 0;
    uart->regs->lcr = LCR_DLAB;
    // fence o,o ?
    uart->regs->dll = 0x01;
    uart->regs->dlm = 0x00;
    // fence o,o ?
    uart->regs->lcr = 0; // DLAB=0

    serial_init(&uart->base, &uart_serial_intf);
    register_device(UART_DEVNAME, DEV_SERIAL, uart);
}
//int uart_serial_open(struct serial *ser)
//Inputs: struct serial *ser - The UART serial device to open (points to uart_serial.base)
//Outputs: int - 0 on success; -EBUSY if the device is already opened; other negative errno on failure (if used)
//Description: Initializes the UART device for use. Resets RX/TX ring buffers, flushes any stale RX data,
//enables the UART interrupt source at a fixed priority, programs the UART IER (e.g., DRIE),
//and marks the device as opened so subsequent opens fail with -EBUSY.
//Side Effects: Touches UART MMIO registers (RBR, IER), enables an interrupt line in the PLIC,
//and modifies internal uart_serial state (opened flag, ring buffers).
int uart_serial_open(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (uart->opened)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // Enable interrupts when data ready (DR) status asserted

    // FIXME your code goes here
    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart);
    uart->regs->ier = IER_DRIE;
    uart ->opened = 1;
    return 0;
}
//void uart_serial_close(struct serial *ser)
//Inputs: struct serial *ser – The UART serial device to close (points to uart_serial.base).
//Outputs: none
//Description: Closes the UART device if it is currently open. Clears UART interrupt enables
//(IER=0), disables the UART’s interrupt source in the PLIC, and marks the device
//as not opened so it can be opened again later. If the device is already closed,
//the function returns immediately.
//Side Effects: Touches UART MMIO registers, disables an external interrupt line, and updates
//internal uart_serial state. Pending RX/TX data may be dropped.

void uart_serial_close(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    // FIXME your code goes here
    
    if(uart->opened == 0){
    	return;
    }
    uart->regs->ier = 0;
    disable_intr_source(uart->irqno);
    uart->opened = 0; 
}

//int uart_serial_recv(struct serial *ser, void *buf, unsigned int bufsz)
//Inputs:
//struct serial *ser  – UART device handle (points to uart_serial.base).
//void *buf           – Destination buffer to copy received bytes into.
//unsigned int bufsz  – Maximum number of bytes to read into buf.
//Outputs:
//int – Number of bytes actually read, or -EINVAL on error
//Description:
//Reads up to bufsz bytes from the UART into buf. If the RX ring is empty,
//waits (via a condition variable) until data arrives; otherwise drains the
//RX ring into buf. Ensures DR interrupts are enabled so incoming data wakes
//blocked readers. Uses a per-UART lock to serialize access.
//Side Effects:
//Enables UART DR interrupt bit (IER_DRIE), blocks on a condition variable,
//acquires/releases a device lock, touches UART MMIO registers, and consumes
//bytes from the RX ring buffer.

int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);
    if (uart->opened == 0 || buf == NULL)
        return -EINVAL;
    if (bufsz == 0)
        return 0;

    int index = uart->irqno - UART0_INTR_SRCNO;
    struct lock *uart_lock = &uart_locks[index];
    lock_acquire(uart_lock);

    unsigned int n = 0;
    uart->regs->ier |= IER_DRIE;

    int pie = disable_interrupts();
    while (n < bufsz) {
        while (rbuf_empty(&uart->rxbuf))
            condition_wait(&uart->rxbnotempty);
        

        while (!rbuf_empty(&uart->rxbuf) && n < bufsz)
            ((char*)buf)[n++] = rbuf_getc(&uart->rxbuf);
    }
    restore_interrupts(pie);

    lock_release(uart_lock);
    return (int)n;
}


//int uart_serial_send(struct serial *ser, const void *buf, unsigned int bufsz)
//Inputs:
//struct serial *ser   – UART device handle (points to uart_serial.base).
//const void *buf      – Source buffer containing bytes to transmit.
//unsigned int bufsz   – Number of bytes to attempt to send.
//Outputs:
//int – Number of bytes actually queued for transmit, or -EINVAL
//Description:
//Writes up to bufsz bytes into the UART TX ring buffer. If the TX ring is
//full, waits (via a condition variable) until space is available, then
//continues. Ensures THRE interrupts are enabled so the ISR drains the TX
//buffer. Uses a per-UART lock to serialize access.
//Side Effects:
//Enables UART THRE interrupt (IER_THREIE), blocks on a condition variable,
//acquires/releases a device lock, touches UART MMIO registers, and appends
//bytes to the TX ring buffer.

int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    struct uart_serial * const uart = (void*)ser - offsetof(struct uart_serial, base);
    if (uart->opened == 0 || buf == NULL) 
	    return -EINVAL;
    if (bufsz == 0) 
	    return 0;
    int index = uart->irqno - 10; //subtract 10 for indexing. UART0 irqno = 10
    struct lock* uart_lock = &uart_locks[index];
    lock_acquire(uart_lock);
    
    unsigned int n = 0;

    int pie = disable_interrupts();
    while(n < bufsz){
	while(rbuf_full(&uart->txbuf))
		condition_wait(&uart->txbnotfull);
	while(!rbuf_full(&uart->txbuf) && n < bufsz)
		rbuf_putc(&uart->txbuf, ((const char*)buf)[n++]);
	uart->regs->ier |= IER_THREIE;
    }
    restore_interrupts(pie);
    lock_release(uart_lock);

    return (int)bufsz;

}
//void uart_isr(int srcno, void *aux)
//Inputs:
//int srcno      – Interrupt source number from the PLIC (unused by handler).
//void *aux      – struct uart_serial* for the UART instance.
//Outputs:
//None.
//Description:
//UART interrupt service routine. Services two interrupt causes:
//- DR (data ready): drains UART RBR into the RX ring buffer until either the
//hardware FIFO is empty or the RX ring is full; broadcasts rxbnotempty.
//- THRE (tx holding register empty): moves bytes from the TX ring buffer to
//the UART THR until either THR is full or the TX ring becomes empty;
//broadcasts txbnotfull.
//Also increments rxovrcnt on overrun (LSR_OE) and updates IER bits:
//disables DRIE when RX ring is full, enables when not; disables THREIE when
//TX ring is empty, enables when not.
//Side Effects:
//Touches UART MMIO registers (LSR, RBR, THR, IER), modifies RX/TX ring
//buffers, signals condition variables

void uart_isr(int srcno, void * aux) {
    // FIXME your code goes here
    struct uart_serial *uart = (struct uart_serial *)aux;

    if (uart->regs->lsr & LSR_OE)
        uart->rxovrcnt++;

    while ((uart->regs->lsr & LSR_DR) && !rbuf_full(&uart->rxbuf)) {
        char c = uart->regs->rbr;
        rbuf_putc(&uart->rxbuf, c);
        condition_broadcast(&uart->rxbnotempty);
    }

    while ((uart->regs->lsr & LSR_THRE) && !rbuf_empty(&uart->txbuf)) {
        char c = rbuf_getc(&uart->txbuf);
        uart->regs->thr = c;
        condition_broadcast(&uart->txbnotfull);
    }

    uint8_t ier = uart->regs->ier;
    if (rbuf_full(&uart->rxbuf))  ier &= (uint8_t)~IER_DRIE;  else ier |= IER_DRIE;
    if (rbuf_empty(&uart->txbuf)) ier &= (uint8_t)~IER_THREIE; else ier |= IER_THREIE;
    uart->regs->ier = ier;
    


}

void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}



int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}


int rbuf_full(const struct ringbuf * rbuf) {
    return (rbuf->tpos - rbuf->hpos == UART_RBUFSZ);
}


void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}
