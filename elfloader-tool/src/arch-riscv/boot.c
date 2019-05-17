/*
 * Copyright 2020, DornerWorks
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <autoconf.h>
#include <elfloader/gen_config.h>

#include <types.h>
#include <binaries/elf/elf.h>
#include <elfloader.h>
#include <abort.h>
#include <cpio/cpio.h>

#define PT_LEVEL_1 1
#define PT_LEVEL_2 2

#define PT_LEVEL_1_BITS 30
#define PT_LEVEL_2_BITS 21

#define PTE_TYPE_TABLE 0x00
#define PTE_TYPE_SRWX 0xCE

#define RISCV_PGSHIFT 12
#define RISCV_PGSIZE BIT(RISCV_PGSHIFT)

// page table entry (PTE) field
#define PTE_V     0x001 // Valid

#define PTE_PPN0_SHIFT 10

#if __riscv_xlen == 32
#define PT_INDEX_BITS  10
#else
#define PT_INDEX_BITS  9
#endif

#define PTES_PER_PT BIT(PT_INDEX_BITS)

#define PTE_CREATE_PPN(PT_BASE)  (unsigned long)(((PT_BASE) >> RISCV_PGSHIFT) << PTE_PPN0_SHIFT)
#define PTE_CREATE_NEXT(PT_BASE) (unsigned long)(PTE_CREATE_PPN(PT_BASE) | PTE_TYPE_TABLE | PTE_V)
#define PTE_CREATE_LEAF(PT_BASE) (unsigned long)(PTE_CREATE_PPN(PT_BASE) | PTE_TYPE_SRWX | PTE_V)

#define GET_PT_INDEX(addr, n) (((addr) >> (((PT_INDEX_BITS) * ((CONFIG_PT_LEVELS) - (n))) + RISCV_PGSHIFT)) % PTES_PER_PT)

#define VIRT_PHYS_ALIGNED(virt, phys, level_bits) (IS_ALIGNED((virt), (level_bits)) && IS_ALIGNED((phys), (level_bits)))

struct image_info kernel_info;
struct image_info user_info;

unsigned long l1pt[PTES_PER_PT] __attribute__((aligned(4096)));
#if __riscv_xlen == 64
unsigned long l2pt[PTES_PER_PT] __attribute__((aligned(4096)));
unsigned long l2pt_elf[PTES_PER_PT] __attribute__((aligned(4096)));
#endif

char elfloader_stack_alloc[BIT(CONFIG_KERNEL_STACK_BITS)];

void map_kernel_window(struct image_info *kernel_info)
{
    uint32_t index;
    unsigned long *lpt;

    /* Map the elfloader into the new address space */
    index = GET_PT_INDEX((uintptr_t)_text, PT_LEVEL_1);

#if __riscv_xlen == 32
    lpt = l1pt;
#else
    lpt = l2pt_elf;
    l1pt[index] = PTE_CREATE_NEXT((uintptr_t)l2pt_elf);
    index = GET_PT_INDEX((uintptr_t)_text, PT_LEVEL_2);
#endif
    uintptr_t _start_rounded = (uintptr_t)_start & ~MASK(21);
    if (IS_ALIGNED((uintptr_t)_start_rounded, PT_LEVEL_2_BITS)) {
        for (int page = 0; index < PTES_PER_PT; index++, page++) {
            lpt[index] = PTE_CREATE_LEAF((uintptr_t)_start_rounded +
        }
    } else {
        printf("Elfloader not properly aligned\n");
        abort();
    }

    /* Map the kernel into the new address space */
    index = GET_PT_INDEX(kernel_info->virt_region_start, PT_LEVEL_1);
    _start_rounded = kernel_info->virt_region_start & ~MASK(21);
    uintptr_t _start_rounded2 = kernel_info->phys_region_start & ~MASK(21);

#if __riscv_xlen == 64
    lpt = l2pt;
    l1pt[index] = PTE_CREATE_NEXT((uintptr_t)l2pt);
    index = GET_PT_INDEX(_start_rounded, PT_LEVEL_2);
#endif
    if (VIRT_PHYS_ALIGNED(_start_rounded,
                          _start_rounded2, PT_LEVEL_2_BITS)) {
        for (int page = 0; index < PTES_PER_PT; index++, page++) {
            lpt[index] = PTE_CREATE_LEAF(_start_rounded2 +
                                         (page << PT_LEVEL_2_BITS));
        }
    } else {
        printf("Kernel not properly aligned\n");
        abort();
    }
}

#if CONFIG_PT_LEVELS == 2
uint64_t vm_mode = 0x1llu << 31;
#elif CONFIG_PT_LEVELS == 3
uint64_t vm_mode = 0x8llu << 60;
#elif CONFIG_PT_LEVELS == 4
uint64_t vm_mode = 0x9llu << 60;
#else
#error "Wrong PT level"
#endif

#if CONFIG_MAX_NUM_NODES > 1
int secondary_go = 0;
int next_logical_core_id = 1;
int mutex = 0;
int core_ready[CONFIG_MAX_NUM_NODES] = { 0 };
static void set_and_wait_for_ready(int hart_id, int core_id)
{
    while (__atomic_exchange_n(&mutex, 1, __ATOMIC_ACQUIRE) != 0);
    printf("Hart ID %d core ID %d\n", hart_id, core_id);
    core_ready[core_id] = 1;
    __atomic_store_n(&mutex, 0, __ATOMIC_RELEASE);

    for (int i = 0; i < CONFIG_MAX_NUM_NODES; i++) {
        while (__atomic_load_n(&core_ready[i], __ATOMIC_RELAXED) == 0) ;
    }
}
#endif

static inline void sfence_vma(void)
{
    asm volatile("sfence.vma" ::: "memory");
}

static inline void ifence(void)
{
    asm volatile("fence.i" ::: "memory");
}

static inline void enable_virtual_memory(void)
{
    sfence_vma();
    asm volatile(
        "csrw satp, %0\n"
        :
        : "r"(vm_mode | (uintptr_t)l1pt >> RISCV_PGSHIFT)
        :
    );
    ifence();
}

void printElfInfo ( struct image_info *info )
{
    printf ( "phys_region_start: %x\n", info->phys_region_start );
    printf ( "phys_region_end  : %x\n", info->phys_region_end );
    printf ( "virt_region_start: %x\n", info->virt_region_start );
    printf ( "virt_region_end  : %x\n", info->virt_region_end );
    printf ( "virt_entry       : %x\n", info->virt_entry );
    printf ( "phys_virt_offset : %x\n", info->phys_virt_offset );
}

int primes() 
{
    unsigned int count = 0;
    unsigned int printCount = 5000;

    for (int i=2; i<10000000; i++) 
    {
        int prime=1;
        for (int j=2; j*j<=i; j++)
        {
            if (i % j == 0) 
            {
                prime=0;
                break;    
            }
        }   
        
        if(prime)
        {
            if (++count % printCount == 0)
            {
                printf("prime: %d\n", i);
            }
        } 
    }

    return 0;
}

void memTest()
{
    printf("sizeof(char): %d\n", sizeof(char));
    volatile char *pMem = (char *)0x40000000;

    for (unsigned int k = 0; k < 256; k++)
    {
        printf("%p, offset: %x, value: %x\n", pMem, k, pMem[k]);
    }
#if 0
    for (unsigned int k = 0; k < 8 * 1024 * 1024; k++)
    {
        if (k % (256 * 1024) == 0)
        {
            printf("%p, offset: %x, value: %x\n", pMem, k, pMem[k]);
        }
    }
#endif
    for (unsigned int k = 0; k < 256; k++)
    {
        volatile char ch = (char)(k % 256);
        
        pMem[k] = ch;
        
        char ch1 = pMem[k];

        printf("%p, offset: %x, written: %d, read: %d\n", pMem, k, ch, ch1);
    }

    for (unsigned int k = 0; k < 64 * 1024 * 1024; k += 1)
    {
        volatile char ch = (char)(k % 256);
#if 0        
        if (((k >= 0x8fc8) && (k <= 0x9200)) || 
            ((k >= 0xe138) && (k <= 0xe238)) ||
            ((k >= 0xe600) && (k <= 0xe800))
            )
        {
            continue;
        }
#endif
        pMem[k] = ch;
        
        char ch1 = pMem[k];

        if (ch != ch1)
        {
            printf("%p, offset: %x, written: %d, read: %d\n", pMem, k, ch, ch1);
        }
//#if 0
        if (k % (1 * 1024) == 0)
        {
            printf("%p, offset: %x\n", pMem, k);
        }
//#endif
        //printf("%p, offset: %x\n", pMem, k);
    }
}

int num_apps = 0;
void main(UNUSED int hartid, void *bootloader_dtb)
{
    void *dtb = NULL;
    uint32_t dtb_size = 0;

    printf("ELF-loader started on (HART %d) (NODES %d)\n", hartid, CONFIG_MAX_NUM_NODES);
    //primes();
    //memTest();

    printf("  paddr=[%p..%p]\n", _text, _end - 1);
    /* Unpack ELF images into memory. */
    load_images(&kernel_info, &user_info, 1, &num_apps, bootloader_dtb, &dtb, &dtb_size);

//#if 0
    if (num_apps != 1) {
        printf("No user images loaded!\n");
        abort();
    }
//#endif

    printf("kernel image info:\n");
    printElfInfo ( &kernel_info);
    printf("user image info:\n");
    printElfInfo(&user_info);

    map_kernel_window(&kernel_info);

    printf("Jumping to kernel-image entry point...\n\n");

#if CONFIG_MAX_NUM_NODES > 1
    /* Unleash secondary cores */
    __atomic_store_n(&secondary_go, 1, __ATOMIC_RELEASE);
    /* Set that the current core is ready and wait for other cores */
    set_and_wait_for_ready(hartid, 0);
#endif

    enable_virtual_memory();

    /* TODO: pass DTB to kernel. */
    ((init_riscv_kernel_t)kernel_info.virt_entry)(user_info.phys_region_start,
                                                  user_info.phys_region_end, user_info.phys_virt_offset,
                                                  user_info.virt_entry
#if CONFIG_MAX_NUM_NODES > 1
                                                  ,
                                                  hartid,
                                                  0
#endif
                                                 );

    /* We should never get here. */
    printf("Kernel returned back to the elf-loader.\n");
}

#if CONFIG_MAX_NUM_NODES > 1

void secondary_entry(int hart_id, int core_id)
{
    while (__atomic_load_n(&secondary_go, __ATOMIC_ACQUIRE) == 0) ;

    set_and_wait_for_ready(hart_id, core_id);

    enable_virtual_memory();

    /* TODO: pass DTB to kernel. */
    ((init_riscv_kernel_t)kernel_info.virt_entry)(user_info.phys_region_start,
                                                  user_info.phys_region_end, user_info.phys_virt_offset,
                                                  user_info.virt_entry
                                                  ,
                                                  hart_id,
                                                  core_id
                                                 );
}

#endif
