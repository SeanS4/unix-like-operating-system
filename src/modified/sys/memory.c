/*! @file memory.c
    @brief Physical and virtual memory manager
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA


*/


#ifdef MEMORY_TRACE
#define TRACE
#endif


#ifdef MEMORY_DEBUG
#define DEBUG
#endif


#include "memory.h"


#include "conf.h"
#include "console.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "process.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"


// COMPILE-TIME CONFIGURATION
//


// Minimum amount of memory in the initial heap block.


#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif


// INTERNAL CONSTANT DEFINITIONS
//


#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE)  // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE)  // gigapage size


#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))


#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif


#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif


// IMPORTED GLOBAL SYMBOLS
//


// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];


// EXPORTED GLOBAL VARIABLES
//


char memory_initialized = 0;


// INTERNAL TYPE DEFINITIONS
//


// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.


/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk *next;  ///< Next page in list
    unsigned long pagecnt;    ///< Number of pages in chunk
};


/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags : 8;
    uint64_t rsw : 2;
    uint64_t ppn : 44;
    uint64_t reserved : 7;
    uint64_t pbmt : 2;
    uint64_t n : 1;
};


// INTERNAL MACRO DEFINITIONS
//


#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2 * 9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1 * 9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0 * 9)) % PTE_CNT)


// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).


#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)


#define PT_INDEX(lvl, vpn) \
    (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//


static void ptab_reset(struct pte *ptab  // page table to reset
);


// static struct pte *ptab_clone(struct pte *ptab  // page table to clone
// );


// static void ptab_discard(struct pte *ptab  // page table to discard
// );


static void ptab_insert(struct pte *ptab,   // page table to modify
                        uintptr_t vma,  // virtual page number to insert
                        void *pp,           // pointer to physical page to insert
                        int rwxug_flags     // flags for inserted mapping
);


// static void *ptab_remove(struct pte *ptab, unsigned long vpn);


// static void ptab_adjust(struct pte *ptab, unsigned long vpn, int rwxug_flags);


struct pte *ptab_fetch(struct pte *ptab, unsigned long vpn);


static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte *root, unsigned int asid);
static inline struct pte *mtag_to_ptab(mtag_t mtag);
static inline struct pte *active_space_ptab(void);


static inline void *pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void *p);
static inline int wellformed(uintptr_t vma);


static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);


// INTERNAL GLOBAL VARIABLES
//


static mtag_t main_mtag;


static struct pte main_pt2[PTE_CNT] __attribute__((section(".bss.pagetable"), aligned(4096)));


static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));


static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));


static struct page_chunk *free_chunk_list;


struct lock page_lock;
struct lock ptab_lock;


// EXPORTED FUNCTION DECLARATIONS
//


void memory_init(void) {
    const void *const text_start = _kimg_text_start;
    const void *const text_end = _kimg_text_end;
    const void *const rodata_start = _kimg_rodata_start;
    const void *const rodata_end = _kimg_rodata_end;
    const void *const data_start = _kimg_data_start;


    void *heap_start;
    void *heap_end;


    uintptr_t pma;
    const void *pp;


    trace("%s()", __func__);


    assert(RAM_START == _kimg_start);


    debug("           RAM: [%p,%p): %zu MB", RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    debug("  Kernel image: [%p,%p)", _kimg_start, _kimg_end);


    // Kernel must fit inside 2MB megapage (one level 1 PTE)


    if (MEGA_SIZE < _kimg_end - _kimg_start) panic(NULL);


    // Initialize main page table with the following direct mapping:
    //
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB


    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void *)pma, PTE_R | PTE_W | PTE_G);


    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);


    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.


    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);


    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }


    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_G);
    }


    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }


    // Remaining RAM mapped in 2MB megapages


    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }


    // Enable paging; this part always makes me nervous.


    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);


    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.


    heap_start = _kimg_end;
    heap_end = (void *)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);


    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP(HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }


    if (RAM_END < heap_end) panic("out of memory");


    // Initialize heap memory manager


    heap_init(heap_start, heap_end);


    debug("Heap allocator: [%p,%p): %zu KB free", heap_start, heap_end,
          (heap_end - heap_start) / 1024);


    // FIXME: Initialize the free chunk list here
    lock_init(&page_lock);
    lock_init(&ptab_lock);
    struct page_chunk * head = (struct page_chunk *) heap_end;
    head->next = NULL;
    head->pagecnt = (RAM_END - heap_end)/PAGE_SIZE;
    free_chunk_list = head;


    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).


    csrs_sstatus(RISCV_SSTATUS_SUM);


    memory_initialized = 1;
}


mtag_t active_mspace(void) { return active_space_mtag(); }


mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;


    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

//recursively clones a page table
static void ptab_clone(struct pte *new, struct pte *curr){
    for(int i = 0; i < PTE_CNT; ++i){
        struct pte *n = &new[i];
        struct pte *c = &curr[i];

        if(!PTE_VALID(*c)){
            *n = null_pte();
            continue;
        }

        if (PTE_GLOBAL(*c)){
            *n = *c;
            continue;
        }

        if(PTE_LEAF(*c)){
            void *currpp = pageptr(c->ppn);
            void *newpp = alloc_phys_page();
            memcpy(newpp, currpp, PAGE_SIZE);
            *n = leaf_pte(newpp, c->flags & (PTE_R | PTE_W | PTE_X | PTE_U));
        }
        else{
            void *newtab = alloc_phys_page();
            memset(newtab, 0, PAGE_SIZE);
            *n = ptab_pte(newtab, 0);
            ptab_clone((struct pte *)newtab,(struct pte *)pageptr(c->ppn));
        }
    }
}


mtag_t clone_active_mspace(void) {
    // FIXME
    lock_acquire(&ptab_lock);

    struct pte *curr = active_space_ptab();
    void *new = alloc_phys_page();
    memset(new, 0 , PAGE_SIZE);

    ptab_clone((struct pte *)new, curr);

    mtag_t mtag = ptab_to_mtag((struct pte *)new, 0);

    lock_release(&ptab_lock);

    return mtag;
}


void reset_active_mspace(void) {
    // FIXME
    lock_acquire(&ptab_lock);
    mtag_t curr = active_mspace();
    struct pte *ptab = mtag_to_ptab(curr);
    ptab_reset(ptab);
    lock_release(&ptab_lock);
    sfence_vma();
}


mtag_t discard_active_mspace(void) {
    // FIXME
    mtag_t reset = active_mspace();
    if(reset == main_mtag) return main_mtag;
    switch_mspace(main_mtag);


    lock_acquire(&ptab_lock);
    struct pte *ptab = mtag_to_ptab(reset);
    ptab_reset(ptab);
    free_phys_page(ptab);
    lock_release(&ptab_lock);


    return main_mtag;
}


// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.


// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.


void *map_page(uintptr_t vma, void *pp, int rwxug_flags) {
    // FIXME
    lock_acquire(&ptab_lock);
    mtag_t curr = active_mspace();
    unsigned long vpn_root = VPN(vma);
    struct pte *ptab_2 = mtag_to_ptab(curr);
    ptab_insert(ptab_2, vpn_root, pp, rwxug_flags);
    lock_release(&ptab_lock);
    // sfence_vma();
    return (void *)vma;
}


static void ptab_insert(struct pte *ptab, uintptr_t vpn, void *pp, int rwxug_flags){
    unsigned long vpn_root = ((vpn >> (2 * 9)) % PTE_CNT);
    struct pte *pte_2 = &ptab[vpn_root];


    struct pte *ptab_1;


    if(!PTE_VALID(*pte_2)){
        void *pp_new = alloc_phys_page();
        memset(pp_new, 0, PAGE_SIZE);
        ptab_1 = (struct pte *) pp_new;
        *pte_2 = ptab_pte(ptab_1, 0);
    }
    else{
        uint64_t ppn_1 = ptab[vpn_root].ppn;
        ptab_1 = (struct pte*)pageptr(ppn_1);
    }


    unsigned long vpn_one = ((vpn >> (1 * 9)) % PTE_CNT);
    struct pte *pte_1 = &ptab_1[vpn_one];


    struct pte *ptab_0;


    if(!PTE_VALID(*pte_1)){
        void *pp_new = alloc_phys_page();
        memset(pp_new, 0, PAGE_SIZE);
        ptab_0 = (struct pte *) pp_new;
        *pte_1 = ptab_pte(ptab_0, 0);
    }
    else{
        uint64_t ppn_0 = ptab_1[vpn_one].ppn;
        ptab_0 = (struct pte *)pageptr(ppn_0);
    }


    unsigned long vpn_zero = ((vpn >> (0 * 9)) % PTE_CNT);
    struct pte *pte_0 = &ptab_0[vpn_zero];


    *pte_0 = leaf_pte(pp, rwxug_flags);
}


void *map_range(uintptr_t vma, size_t size, void *pp, int rwxug_flags) {
    // FIXME
    for(size_t i = 0; i < size; i += PAGE_SIZE){
        uintptr_t curr_vma = vma + i;
        void *curr_pp = (void*)((uintptr_t)pp + i);
        map_page(curr_vma, curr_pp, rwxug_flags);
    }
    sfence_vma();
    return (void *)vma;
}


void *alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    // FIXME
    if((vma % PAGE_SIZE) != 0) panic("vma not aligned with PAGE_SIZE");
    unsigned int cnt = (size + PAGE_SIZE - 1)/PAGE_SIZE;
    size_t size_new = cnt * PAGE_SIZE;
    void *pp = alloc_phys_pages(cnt);
    if(pp == NULL) return NULL;
    map_range(vma, size_new, pp, rwxug_flags);
    return (void*) vma;
}


void set_range_flags(const void *vp, size_t size, int rwxug_flags) {
    // FIXME
    lock_acquire(&ptab_lock);
    struct pte *ptab_2 = active_space_ptab();
    if(((uintptr_t)vp % PAGE_SIZE) != 0){
        lock_release(&ptab_lock);
        panic("vp isn't page aligned in set_range_flags");
    }


    for(size_t i = 0; i < size; i += PAGE_SIZE){
        unsigned long vpn_2 = VPN2((uintptr_t)vp+i);
        struct pte *pte_2 = &ptab_2[vpn_2];
        if(PTE_VALID(*pte_2) && !PTE_LEAF(*pte_2)){
            uint64_t ppn_1 = pte_2->ppn;
            struct pte *ptab_1 = (struct pte*) pageptr(ppn_1);
            unsigned long vpn_1 = VPN1((uintptr_t)vp+i);
            struct pte *pte_1 = &ptab_1[vpn_1];
            if(PTE_VALID(*pte_1) && !PTE_LEAF(*pte_1)){
                uint64_t ppn_0 = pte_1->ppn;
                struct pte *ptab_0 = (struct pte*) pageptr(ppn_0);
                unsigned long vpn_0 = VPN0((uintptr_t)vp+i);
                struct pte *pte_0 = &ptab_0[vpn_0];
                if(PTE_VALID(*pte_0)){
                    uint64_t ppn = pte_0->ppn;
                    void *pp = pageptr(ppn);
                    *pte_0 = leaf_pte(pp, rwxug_flags);
                }
            }
        }
    }
    lock_release(&ptab_lock);
    sfence_vma();
}


void unmap_and_free_range(void *vp, size_t size) {
    // FIXME
    lock_acquire(&ptab_lock);
    struct pte *ptab_2 = active_space_ptab();
   
    for(size_t i = 0; i < size; i += PAGE_SIZE){
        unsigned long vpn_2 = VPN2((uintptr_t)vp+i);
        struct pte *pte_2 = &ptab_2[vpn_2];
        if(PTE_VALID(*pte_2) && !PTE_LEAF(*pte_2)){
            uint64_t ppn_1 = pte_2->ppn;
            struct pte *ptab_1 = (struct pte*) pageptr(ppn_1);
            unsigned long vpn_1 = VPN1((uintptr_t)vp+i);
            struct pte *pte_1 = &ptab_1[vpn_1];
            if(PTE_VALID(*pte_1) && !PTE_LEAF(*pte_1)){
                uint64_t ppn_0 = pte_1->ppn;
                struct pte *ptab_0 = (struct pte*) pageptr(ppn_0);
                unsigned long vpn_0 = VPN0((uintptr_t)vp+i);
                struct pte *pte_0 = &ptab_0[vpn_0];
                if(PTE_VALID(*pte_0)){
                    uint64_t ppn = pte_0->ppn;
                    void *pp = pageptr(ppn);
                    *pte_0 = null_pte();
                    free_phys_page(pp);
                }
            }
        }
    }
    lock_release(&ptab_lock);
    sfence_vma();
}


int validate_vptr(const void *vp, size_t len, int rwxu_flags) {
    // FIXME
    lock_acquire(&ptab_lock);
    if(((uintptr_t)vp + len) < (uintptr_t)vp){
        lock_release(&ptab_lock);
        return -ENOMEM;
    }


    struct pte *ptab_2 = active_space_ptab();
   
    // for(size_t i = 0; i < (uintptr_t) vp+len; i += PAGE_SIZE){
    uintptr_t end = (uintptr_t)vp + len;
    uintptr_t curr = ((uintptr_t)vp / PAGE_SIZE) * PAGE_SIZE;
    while(curr < end){
        if(!wellformed((curr))){
            lock_release(&ptab_lock);
            return -EBADFMT;
        }
        unsigned long vpn_2 = VPN2(curr);
        struct pte *pte_2 = &ptab_2[vpn_2];
        if(PTE_VALID(*pte_2) && !PTE_LEAF(*pte_2)){
            uint64_t ppn_1 = pte_2->ppn;
            struct pte *ptab_1 = (struct pte*) pageptr(ppn_1);
            unsigned long vpn_1 = VPN1(curr);
            struct pte *pte_1 = &ptab_1[vpn_1];
            if(PTE_VALID(*pte_1) && !PTE_LEAF(*pte_1)){
                uint64_t ppn_0 = pte_1->ppn;
                struct pte *ptab_0 = (struct pte*) pageptr(ppn_0);
                unsigned long vpn_0 = VPN0(curr);
                struct pte *pte_0 = &ptab_0[vpn_0];
                if(PTE_VALID(*pte_0) && PTE_LEAF(*pte_0)){
                    if((pte_0->flags & rwxu_flags) != rwxu_flags){
                        lock_release(&ptab_lock);
                        return -EINVAL;
                    }
                }
                else{
                    lock_release(&ptab_lock);
                    return -EBADFMT;
                }
            }
            else{
                lock_release(&ptab_lock);
                return -EBADFMT;
            }
        }
        else{
            lock_release(&ptab_lock);
            return -EBADFMT;
        }
        curr += PAGE_SIZE;
    }
    lock_release(&ptab_lock);
    return 0;
}


int validate_vstr(const char *vs, int rug_flags) {
    // FIXME
    lock_acquire(&ptab_lock);
    uintptr_t vma = (uintptr_t) vs;
    struct pte *ptab_2 = active_space_ptab();
   
    while(1){
        if(!wellformed(vma)){
            lock_release(&ptab_lock);
            return -EBADFMT;
        }
        unsigned long vpn_2 = VPN2((uintptr_t)vma);
        struct pte *pte_2 = &ptab_2[vpn_2];
        if(PTE_VALID(*pte_2) && !PTE_LEAF(*pte_2)){
            uint64_t ppn_1 = pte_2->ppn;
            struct pte *ptab_1 = (struct pte*) pageptr(ppn_1);
            unsigned long vpn_1 = VPN1(vma);
            struct pte *pte_1 = &ptab_1[vpn_1];
            if(PTE_VALID(*pte_1) && !PTE_LEAF(*pte_1)){
                uint64_t ppn_0 = pte_1->ppn;
                struct pte *ptab_0 = (struct pte*) pageptr(ppn_0);
                unsigned long vpn_0 = VPN0(vma);
                struct pte *pte_0 = &ptab_0[vpn_0];
                if(PTE_VALID(*pte_0) && PTE_LEAF(*pte_0)){
                    if((pte_0->flags & rug_flags) != rug_flags){
                        lock_release(&ptab_lock);
                        return -EINVAL;
                    }
                    void *pp = pageptr(pte_0->ppn);
                    size_t offset = (vma % PAGE_SIZE);
                    for(size_t i = offset; i < PAGE_SIZE; ++i){
                        char c = ((char *)pp)[i];
                        if(c == '\0'){
                            lock_release(&ptab_lock);
                            return 0;
                        }
                    }
                    vma = (vma - (vma % PAGE_SIZE)) + PAGE_SIZE;
                }
                else{
                    lock_release(&ptab_lock);
                    return -EBADFMT;
                }
            }
            else{
                lock_release(&ptab_lock);
                return -EBADFMT;
            }
        }
        else{
            lock_release(&ptab_lock);
            return -EBADFMT;
        }
    }
}


void *alloc_phys_page(void) {
    // FIXME
    void *pp = alloc_phys_pages(1); //allocates single page using alloc_phys_pages
    return pp;
}


void free_phys_page(void *pp) {
    // FIXME
    free_phys_pages(pp, 1); //free single page using free_phys_pages
}


void *alloc_phys_pages(unsigned int cnt) {
    // FIXME
    lock_acquire(&page_lock);
    if(free_chunk_list == NULL){
        lock_release(&page_lock);
        panic("free chunk list is empty");
    }


    struct page_chunk *curr = free_chunk_list;
    struct page_chunk *prev = NULL;
    struct page_chunk *best = NULL;
    if(curr->pagecnt >= cnt){
        best = curr;
        }
    while(curr->next != NULL){
        if((best == NULL && curr->next->pagecnt >= cnt)|| (curr->next->pagecnt >= cnt && curr->next->pagecnt < best->pagecnt)){
            prev = curr;
            best = curr->next;
        }
        curr = curr->next;
    }
    void *pp = NULL;
    if(best != NULL){
        if(best->pagecnt == cnt){
            if(prev != NULL){
                prev->next = best->next;
                best->next = NULL;
                pp = best;
            }
            else{
                pp = best;
                free_chunk_list = best->next;
            }
        }


        if(best->pagecnt > cnt){
            pp = (void *)((uintptr_t)best + ((best->pagecnt - cnt) * PAGE_SIZE)); //gets last page from the best chunk available
            best->pagecnt -= cnt;
        }
    }
    if(pp == NULL){
        lock_release(&page_lock);
        panic("no valid chunk found for page allocation");
    }
    lock_release(&page_lock);
    return pp;
}


void free_phys_pages(void *pp, unsigned int cnt) {
    // FIXME
    if(pp == NULL || cnt == 0) return;
    lock_acquire(&page_lock);
    if(free_chunk_list == NULL){
        struct page_chunk *new = (struct page_chunk *)pp;
        new->next = NULL;
        new->pagecnt = cnt;
        free_chunk_list = new;
        lock_release(&page_lock);
        return;
    }


    struct page_chunk *curr = free_chunk_list;


    while(curr != NULL){
        uintptr_t address = 0;
        address = (uintptr_t)curr + (curr->pagecnt * PAGE_SIZE);
        if(address == (uintptr_t) pp){
            // struct page_chunk *new = (struct page_chunk *)pp;
            // new->pagecnt = cnt;
            // new->next = curr->next;
            // curr->next = new;
            curr->pagecnt += cnt;
            lock_release(&page_lock);
            return;
        }
        curr = curr->next;
    }
    struct page_chunk *new = (struct page_chunk *)pp;
    new->next = free_chunk_list;
    new->pagecnt = cnt;
    free_chunk_list = new;
    lock_release(&page_lock);
}


unsigned long free_phys_page_count(void) {
    // FIXME
    lock_acquire(&page_lock);
    struct page_chunk *curr = free_chunk_list;
    unsigned long cnt = 0;


    while(curr != NULL){
        cnt += curr->pagecnt;
        curr = curr->next;
    }
    lock_release(&page_lock);
    return cnt;
}


int handle_umode_page_fault(struct trap_frame *tfr, uintptr_t vma) {
    // FIXME
    if(!wellformed(vma)) return 0;
    if(vma < UMEM_START_VMA || vma >= UMEM_END_VMA) return 0;
    void *pp = alloc_phys_page();
    if(pp == NULL){
        //panic("No more space in memory");
        return 0;
    }
    memset(pp, 0, PAGE_SIZE);
    map_page(vma, pp, PTE_U | PTE_R | PTE_W);
    sfence_vma();
    return 1;
}


/**
 * @brief Reads satp to retrieve tag for active memory space
 * @return Tag for active memory space
 */
mtag_t active_space_mtag(void) { return csrr_satp(); }


/**
 * @brief Constructs tag from page table address and address space identifier
 * @param ptab Pointer to page table to use in tag
 * @param asid Address space identifier to use in tag
 * @return Memory tag formed from paging mode, page table address, and ASID
 */
static inline mtag_t ptab_to_mtag(struct pte *ptab, unsigned int asid) {
    return (((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
            ((unsigned long)asid << RISCV_SATP_ASID_shift) | pagenum(ptab) << RISCV_SATP_PPN_shift);
}


/**
 * @brief Retrives a page table address from a tag
 * @param mtag Tag to extract page table address from
 * @return Pointer to page table retrieved from tag
 */
static inline struct pte *mtag_to_ptab(mtag_t mtag) { return (struct pte *)((mtag << 20) >> 8); }


/**
 * @brief Returns the address of the page table corresponding to the active memory space
 * @return Pointer to page table extracted from active memory space tag
 */
static inline struct pte *active_space_ptab(void) { return mtag_to_ptab(active_space_mtag()); }


/**
 * @brief Constructs a physical pointer from a physical page number
 * @param n Physical page number to derive physical pointer from
 * @return Pointer to memory corresponding to physical page
 */
static inline void *pageptr(uintptr_t n) { return (void *)(n << PAGE_ORDER); }


/**
 * @brief Constructs a physical page number from a pointer
 * @param p Pointer to derive physical page number from
 * @return Physical page number corresponding to pointer
 */
static inline unsigned long pagenum(const void *p) { return (unsigned long)p >> PAGE_ORDER; }


/**
 * @brief Checks if bits 63:38 of passed virtual memory address are all 1 or all 0
 * @param vma Virtual memory address to check well-formedness of
 * @return 1 if pointer is well-formed, 0 otherwise
 */
static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits + 1));
}


/**
 * @brief Constructs a page table entry corresponding to a leaf
 * @details For our purposes, a leaf PTE has the A, D, and V flags set
 * @param pp Physical address to set physical page number of PTE from
 * @param rwxug_flags Flags to set on PTE
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags) {
    return (struct pte){.flags = rwxug_flags | PTE_A | PTE_D | PTE_V, .ppn = pagenum(pp)};
}


/**
 * @brief Constructs a page table entry corresponding to a page table
 * @param pt Physical address to set physical page number of PTE from
 * @param g_flag Flags to set on PTE (should either be G flag or nothing)
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag) {
    return (struct pte){.flags = g_flag | PTE_V, .ppn = pagenum(pt)};
}


/**
 * @brief Returns an empty pte
 * @return An empty pte
 */
static inline struct pte null_pte(void) { return (struct pte){}; }


/**
 * @brief Unmaps and frees all non-global pages in the pte
 */
static void ptab_reset(struct pte *ptab){
    for(int i = 0; i < PTE_CNT; ++i){
        struct pte * curr = &ptab[i];
        if(PTE_VALID(*curr) && !PTE_GLOBAL(*curr)){
            if(PTE_LEAF(*curr)){
                void *pp = pageptr(curr->ppn);
                free_phys_page(pp);
                *curr = null_pte();
            }
            else{
                void *pp = pageptr(curr->ppn);
                struct pte * next = (struct pte *) pp;
                ptab_reset(next);
                free_phys_page(pp);
                *curr = null_pte();
            }
        }
    }
}