/*! @file main.c
 *  @brief Main function of the kernel, called from start.s
 *  @copyright Copyright (c) 2024-2025 University of Illinois
 */

#include "cache.h"
#include "conf.h"
#include "console.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "memory.h"
#include "process.h"
#include "thread.h"
#include "uio.h"

#define INITEXE "shell"

#define CMNTNAME   "c"
#define DEVMNTNAME "dev"
#define CDEVNAME   "vioblk"
#define CDEVINST   0

#ifndef NUART
#define NUART 2
#endif

#ifndef NVIODEV
#define NVIODEV 8
#endif

static void attach_devices(void);
static void mount_cdrive(void);
static void run_init(void);

void main(void) {
    console_init();
    intrmgr_init();
    devmgr_init();
    thrmgr_init();
    memory_init();
    procmgr_init();

    attach_devices();
    enable_interrupts();
    mount_cdrive();

    run_init();

    /* process_exec should not return after a successful exec. */
    halt_failure();
}

static void attach_devices(void) {
    int result;

    rtc_attach((void *)RTC_MMIO_BASE);

    for (int i = 0; i < NUART; i++) {
        attach_uart(
            (void *)UART_MMIO_BASE(i),
            UART0_INTR_SRCNO + i
        );
    }

    for (int i = 0; i < NVIODEV; i++) {
        attach_virtio(
            (void *)VIRTIO_MMIO_BASE(i),
            VIRTIO0_INTR_SRCNO + i
        );
    }

    result = mount_devfs(DEVMNTNAME);
    if (result != 0) {
        kprintf(
            "mount_devfs(%s) failed: %s\n",
            DEVMNTNAME,
            error_name(result)
        );
        halt_failure();
    }
}

static void mount_cdrive(void) {
    struct storage *hd;
    struct cache *cache;
    int result;

    hd = find_storage(CDEVNAME, CDEVINST);
    if (hd == NULL) {
        kprintf(
            "Storage device %s%d not found\n",
            CDEVNAME,
            CDEVINST
        );
        halt_failure();
    }

    result = storage_open(hd);
    if (result != 0) {
        kprintf(
            "storage_open failed on %s%d: %s\n",
            CDEVNAME,
            CDEVINST,
            error_name(result)
        );
        halt_failure();
    }

    result = create_cache(hd, &cache);
    if (result != 0) {
        kprintf(
            "create_cache(%s%d) failed: %s\n",
            CDEVNAME,
            CDEVINST,
            error_name(result)
        );
        halt_failure();
    }

    result = mount_ktfs(CMNTNAME, cache);
    if (result != 0) {
        kprintf(
            "mount_ktfs(%s, cache(%s%d)) failed: %s\n",
            CMNTNAME,
            CDEVNAME,
            CDEVINST,
            error_name(result)
        );
        halt_failure();
    }
}

static void run_init(void) {
    struct uio *initexe = NULL;
    int result;

    result = open_file(CMNTNAME, INITEXE, &initexe);
    if (result != 0) {
        kprintf(
            "%s/%s: %s; terminating\n",
            CMNTNAME,
            INITEXE,
            error_name(result)
        );
        halt_failure();
    }

    /*
     * On success, process_exec transitions the current main thread
     * into U-mode and begins executing c/shell. It should not return.
     *
     * The working test suite starts shell with argc=0 and argv=NULL,
     * so preserve that known-good setup here.
     */
    result = process_exec(initexe, 0, NULL);

    /* Reaching here means exec failed before entering user mode. */
    kprintf(
        "process_exec(%s/%s) returned: %s (%d)\n",
        CMNTNAME,
        INITEXE,
        error_name(result),
        result
    );

    halt_failure();
}