// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "misc.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 


struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT]; /**< Interrupt Priorities registers */
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32]; /**< Interrupt Pending Bits registers */
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32]; /**< Interrupt Enables registers */
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;	/**< Priority Thresholds registers */
				uint32_t claim;	/**< Interrupt Claim/Completion registers */
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);

static int plic_source_pending(uint_fast32_t srcno);

static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);

static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);

static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);


static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
//	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//
//void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
//Inputs:
//uint_fast32_t srcno - Interrupt source number in the PLIC.
//uint_fast32_t level - Priority level to assign to that source.
//Outputs:
//None
//Description:
//Programs the PLIC priority for the given
//interrupt source by writing the priority register entry for srcno to level.
//Side Effects:
//If the level is 0, will disable the source
static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	// FIXME your code goes here
	PLIC.priority[srcno] = level;
}
//int plic_source_pending(uint_fast32_t srcno)
//Inputs:
//uint_fast32_t srcno - Interrupt source number in the PLIC.
//Outputs:
//int - 1 if the source has a pending interrupt, 0 otherwise.
//Description:
//Checks the PLIC pending bitmap for the given source and returns whether that
//interrupt source is currently pending.
//Side Effects:

static inline int plic_source_pending(uint_fast32_t srcno) {
	// FIXME your code goes here
	if(PLIC.pending[srcno/32] & (1u << (srcno % 32))){
		return 1;		
	}
	return 0;
}
//void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
//Inputs:
//uint_fast32_t ctxno - PLIC context (hart/mode) to configure.
//uint_fast32_t srcno - Interrupt source number to enable for that context.
//Outputs:
//None
//Description:
//Enables the specified interrupt source for the given PLIC context by setting
//the corresponding bit in the context’s enable bitmap.
//Side Effects:

static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
	PLIC.enable[ctxno][srcno/32] |= 1u << (srcno%32);
}
//void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
//Inputs:
//uint_fast32_t ctxno - PLIC context (hart/mode) whose mask is being modified.
//uint_fast32_t srcid - Interrupt source number to disable for that context.
//Outputs:
//None
//Description:
//Disables the specified interrupt source for the given PLIC context by clearing
//the corresponding bit in that context’s enable bitmap.
//Side Effects:
//Prevents a source from being delivered a context.
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
	// FIXME your code goes here
	PLIC.enable[ctxno][srcid/32] &= ~(1u << (srcid%32));
}
//void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
//Inputs:
//uint_fast32_t ctxno - PLIC context (hart/mode) whose priority threshold to set.
//uint_fast32_t level - Threshold level; only interrupts with priority > level are delivered.
//Outputs:
//None
//Description:
//Programs the PLIC context’s threshold register so that interrupts at or below
//this level are masked for the specified context.
//Side Effects:
static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
	// FIXME your code goes here
	PLIC.ctx[ctxno].threshold = level;
}
//uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
//Inputs:
//uint_fast32_t ctxno – PLIC context (hart/mode) whose pending interrupt should be claimed.
//Outputs:
//uint_fast32_t – The source ID of the highest-priority pending interrupt for this context,
//or 0 if none are pending.
//Description:
//Reads the PLIC claim/complete register for the given context to claim (atomically fetch)
//the next deliverable interrupt source. The returned ID must later be written back to the
//same register to signal completion.
//Side Effects:
//Consumes the pending interrupt for this context in the PLIC until it is completed by a
//matching write to the claim register.
static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	// FIXME your code goes here
	return PLIC.ctx[ctxno].claim;
	
}
//void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
//Inputs: 
//uint_fast32_t ctxno - PLIC context (hart/mode) that claimed the interrupt
//uint_fast32_t srcno - Interrupt source ID being completed
//Outputs: 
//None
//Description: 
//Writes srcno to the PLIC claim/complete register for ctxno to signal completion.
//Side Effects: 
//Clears the claim for that source in the given context; may allow other pending interrupts.
static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
	PLIC.ctx[ctxno].claim = srcno;
}
//void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
//Inputs: 
//uint_fast32_t ctxno - PLIC context (hart/mode) whose enable bitmap will be set
//Outputs: 
//None
//Description: 
//Enables all interrupt sources for the given PLIC context by setting every bit
//Side Effects: 
//None

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	for(int i = 0; i <  32; i++){
		PLIC.enable[ctxno][i] = 0xFFFFFFFFu;
	}
}
//void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
//Inputs: 
//uint_fast32_t ctxno - PLIC context (hart/mode) whose enable bitmap will be cleared
//Outputs: 
//None
//Description: Disables all interrupt sources for the given PLIC context by clearing every bit
//in that context’s enable array.
//Side Effects: 
//None

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	for(int i = 0; i < 32; i++){
		PLIC.enable[ctxno][i] = 0u;
	}
}
