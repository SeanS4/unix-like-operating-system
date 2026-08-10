// timer.c - A timer system
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif
#include <stddef.h>
#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "misc.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//
static unsigned long long ticks;
static unsigned long long next_tick;
static struct alarm * sleep_list;

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}

void alarm_init(struct alarm * al, const char * name) {
    // FIXME your code goes here
    al->twake = rdtime();
    if(name == NULL)
	condition_init(&al->cond, "alarm");    
    else
	condition_init(&al->cond, name);
    al->next = NULL;
}

void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    struct alarm * prev;
    int pie;

    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;
    
    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;
    
    // FIXME your code goes here
    al->next = NULL;

    pie = disable_interrupts();
    int was_empty = (sleep_list == NULL);
    struct alarm * curr;
    if (was_empty || al->twake <= sleep_list->twake) {
        al->next = sleep_list;
        sleep_list = al;
    } else {
        prev = sleep_list;
        curr = sleep_list->next;
        while (curr != NULL && curr->twake < al->twake) {
            prev = curr;
            curr = curr->next;
        }
        al->next = curr;
        prev->next = al;
    }

    set_stcmp(sleep_list->twake);

    if (was_empty)
        csrs_sie(1UL << 5);

    while (rdtime() < al->twake)
        condition_wait(&al->cond);

    restore_interrupts(pie);
}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

void handle_timer_interrupt(void) {
    struct alarm * head = sleep_list;
    struct alarm * next;
    uint64_t now;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    // FIXME your code goes here
    while (head && head->twake <= now) {
        next = head->next;
        sleep_list = next;
        condition_broadcast(&head->cond);
        head = next;
    }

    if(ticks == 0){
    	ticks = (TIMER_FREQ/100);
    }

    if(now >= next_tick){
    	unsigned long long q = ticks;
	unsigned long long step = ((now - next_tick)/q)+1;
	next_tick = next_tick + step * q;
    }

    unsigned long long next_compare = next_tick;


	


    if (sleep_list && sleep_list->twake < next_compare)
    	next_compare = sleep_list->twake;    
    set_stcmp(next_compare);
   
    csrc_sie(1UL << 5);

}
