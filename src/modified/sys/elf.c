/*! @file elf.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌​​‌​⁠⁠‌
    @brief ELF file loader
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef ELF_TRACE
#define TRACE
#endif

#ifdef ELF_DEBUG
#define DEBUG
#endif

#include "elf.h"

#include <stdint.h>

#include "conf.h"
#include "error.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "uio.h"
#include "console.h"
#include "memory.h"

// Offsets into e_ident

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9

// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE 0
#define EV_CURRENT 1

// ELF header e_type values

enum elf_et { ET_NONE = 0, ET_REL, ET_EXEC, ET_DYN, ET_CORE };

/*! @struct elf64_ehdr
    @brief ELF header struct
*/
struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

/*! @enum elf_pt
    @brief Program header p_type values
*/
enum elf_pt { PT_NULL = 0, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_SHLIB, PT_PHDR, PT_TLS };

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/*! @struct elf64_phdr
    @brief Program header struct
*/
struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define EM_RISCV 243
/**
 * \brief Validates and loads an ELF file into memory.
 *
 * This function validates an ELF file, then loads its contents into memory,
 * returning the start of the entry point through \p eptr.
 *
 * The loader processes only program header entries of type `PT_LOAD`. The layouts
 * of structures and magic values can be found in the Linux ELF header file
 * `<uapi/linux/elf.h>`
 * The implementation should ensure that all loaded sections of the program are
 * mapped within the memory range `0x80100000` to `0x81000000`.
 *
 * Let's do some reading! The following documentation will be very helpful!
 * [Helpful doc](https://linux.die.net/man/5/elf)
 * Good luck!
 * [Educational video](https://www.youtube.com/watch?v=dQw4w9WgXcQ)
 *
 * \param[in]  uio  Pointer to an user I/O corresponding to the ELF file.
 * \param[out] eptr   Double pointer used to return the ELF file's entry point.
 *
 * \return 0 on success, or a negative error code on failure.
 */
int elf_load(struct uio* uio, void (**eptr)(void)) {
    // FIXME
    struct elf64_ehdr header;
    
    long bytes_h = uio_read(uio, &header, sizeof(struct elf64_ehdr));
    

    if(bytes_h < 0)
        return (int)bytes_h;
    if((unsigned long)bytes_h != sizeof(struct elf64_ehdr)){
         return -EINVAL;
    }
    if(header.e_ident[0] != 0x7F || header.e_ident[1] != 'E' || header.e_ident[2] != 'L' || header.e_ident[3] != 'F'){
        return -EINVAL;
    }


    if(header.e_ident[EI_CLASS] != ELFCLASS64) return -EINVAL;

    if(header.e_ident[EI_DATA] != ELFDATA2LSB) return -EINVAL;

    if(header.e_ident[EI_VERSION] != EV_CURRENT) return -EINVAL;

    if(header.e_type != ET_EXEC) return -EINVAL;

    if(header.e_machine != EM_RISCV) return -EINVAL;

    if(header.e_version != EV_CURRENT) return -EINVAL;

    if(header.e_ehsize != sizeof(struct elf64_ehdr)) return -EINVAL;

    if(header.e_phentsize != sizeof(struct elf64_phdr)) return -EINVAL;


    *eptr = (void (*) (void))header.e_entry;


    long bytes_p;
    int rc;
    struct elf64_phdr program;
    for(int i = 0; i < header.e_phnum; ++i){
        uint64_t offset = header.e_phoff + i*header.e_phentsize;
        
        rc = uio_cntl(uio, FCNTL_SETPOS, &offset);
        
        if(rc < 0) return rc;

        
        bytes_p = uio_read(uio, &program, header.e_phentsize);
        
        if(bytes_p < 0) return (int)bytes_p;
        if((unsigned long)bytes_p != header.e_phentsize) return -EINVAL;

        if(program.p_type == PT_LOAD) {
            uint64_t start = program.p_vaddr;
            uint64_t end = program.p_vaddr + program.p_memsz;
            if(start < UMEM_START_VMA || end > UMEM_END_VMA || end < start) return -EINVAL;

            uint64_t flags = PTE_U | PTE_R | PTE_W; 

            size_t offset = start % PAGE_SIZE; //offset in the page
            size_t data = program.p_memsz + offset;
            size_t page_size;
            if((data % PAGE_SIZE) == 0) page_size = data; //keep page alligned
            else
                page_size = (data/PAGE_SIZE + 1) * PAGE_SIZE;

            void *pp = alloc_and_map_range((uintptr_t)(start - offset), page_size, flags); //need to start at the beginning of a page
            if(pp == NULL) return -ENOMEM;
            
            void *pp_addr = (void *)((uintptr_t)pp + offset);
            
            rc = uio_cntl(uio, FCNTL_SETPOS, &program.p_offset);
            
            if(rc < 0) return rc;

            bytes_p = uio_read(uio, pp_addr, program.p_filesz);
            
            if(bytes_p < 0) return (int)bytes_p;
            if((unsigned long)bytes_p != program.p_filesz) return -EINVAL;

            if(program.p_memsz > program.p_filesz){
                memset((void *)((char *)pp_addr + program.p_filesz), 0, program.p_memsz - program.p_filesz); //cast pp_addr to char so we increment 1 byte at a time
            }
            flags = PTE_U; 
            if(program.p_flags & PF_R){
                flags |= PTE_R;
            }
            if(program.p_flags & PF_X){
                flags |= PTE_X;
            }
            if(program.p_flags & PF_W){
                flags |= PTE_W;
            }
            set_range_flags((void *)(start - offset), page_size, flags);
        }
    }
    return 0;
}
