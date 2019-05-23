/*
 * Copyright 2018, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * Copyright 2018, DornerWorks
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_DORNERWORKS_GPL)
 */
#include <autoconf.h>

#include <types.h>
#include <binaries/elf/elf.h>
#include <elfloader.h>
#include <platform.h>
#include <abort.h>
#include <cpio/cpio.h>

#include "../plat/spike/sbi.h"

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
char elfloader_stack_alloc[BIT ( CONFIG_KERNEL_STACK_BITS )];

void map_kernel_window(struct image_info *kernel_info)
{
    uint32_t index;
    unsigned long *lpt;

    /* Map the elfloader into the new address space */
    index = GET_PT_INDEX((uintptr_t)_start, PT_LEVEL_1);

#if __riscv_xlen == 32
    lpt = l1pt;
#else
    lpt = l2pt_elf;
    l1pt[index] = PTE_CREATE_NEXT((uintptr_t)l2pt_elf);
    index = GET_PT_INDEX((uintptr_t)_start, PT_LEVEL_2);
#endif
    uintptr_t _start_rounded = (uintptr_t)_start & ~MASK(21);
    if (IS_ALIGNED((uintptr_t)_start_rounded, PT_LEVEL_2_BITS)) {
        for (int page = 0; index < PTES_PER_PT; index++, page++) {
            lpt[index] = PTE_CREATE_LEAF((uintptr_t)_start_rounded +
                                         (page << PT_LEVEL_2_BITS));
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

extern void _elf_loader_trap_entry ( void );

char _elf_loader_sscratch[4096];

static inline void write_sscratch ( word_t value )
{
    printf ( "write_sscratch: %0x\n", value );
    asm volatile ( "csrw sscratch, %0" :: "rK" ( value ) );
}

static inline void write_stvec ( word_t value )
{
    word_t regValue;

    asm volatile (
        "csrr %0, stvec"
        :"=r" ( regValue )
    );
    printf ( "current_stvec: %0x\n", regValue );

    printf ( "write_stvec: %0x\n", value );
    asm volatile ( "csrw stvec, %0" :: "rK" ( value ) );

    asm volatile (
        "csrr %0, stvec"
        :"=r" ( regValue )
    );
    printf ( "current_stvec: %0x\n", regValue );
}

static void init_cpu ( void )
{
    /* Write trap entry address to stvec */
    write_stvec ( ( word_t ) _elf_loader_trap_entry );

    write_sscratch ( ( word_t ) _elf_loader_sscratch );
}

static inline void set_sie_mask ( word_t mask_high )
{
    word_t temp;
    asm volatile ( "csrrs %0, sie, %1" : "=r" ( temp ) : "rK" ( mask_high ) );
}

#define MS_IN_S     1000llu
#define CONFIG_TIMER_TICK_MS 2

#define RESET_CYCLES ((CONFIG_SPIKE_CLOCK_FREQ / MS_IN_S) * CONFIG_TIMER_TICK_MS)

static inline uint64_t get_cycles ( void )
#if __riscv_xlen == 32
{
    uint32_t nH, nL;
    asm volatile (
        "rdtimeh %0\n"
        "rdtime  %1\n"
        : "=r" ( nH ), "=r" ( nL ) );
    return ( ( uint64_t ) ( ( uint64_t ) nH << 32 ) ) | ( nL );
}
#else
{
    uint64_t n;
    asm volatile (
        "rdtime %0"
        : "=r" ( n ) );
    return n;
#endif
}

void printCycle ( uint64_t time )
{
    long long x = time;
    printf ( "%0x%0x\n", ( long ) ( x >> 32 ), ( long ) ( x & 0xffffffff ) );
}

void initTimer (unsigned int interruptDelay)
{
    uint64_t triggerTime = get_cycles() + interruptDelay;
    //sbi_set_timer(get_cycles() + RESET_CYCLES);
    printCycle ( triggerTime );
    sbi_set_timer ( triggerTime );
    printCycle ( get_cycles() );
    printf ( "initTime done\n" );
}

static inline void clear_sip_mask ( word_t mask_low )
{
    word_t temp;
    asm volatile ( "csrrc %0, sip, %1" : "=r" ( temp ) : "rK" ( mask_low ) );
}

#if 0
#endif

word_t interuptAcknowledge;
word_t readSscratch;

int num_apps = 0;


#define DECAY_DELAY
#define KICK_INTERRUPT
#define DECAY_INTERRUPT

void main ( int hardid, unsigned long dtb )
{
    word_t sstatus_reg;
    word_t sip_reg;
    word_t sie_reg;
    word_t scause_reg;


    uint32_t *returnValue = ( uint32_t * ) 0x1007ffc;
    *returnValue = 0xaabbccdd;

    printf ( "ELF-loader started on\n" );

    uint32_t *returnValue1 = ( uint32_t * ) 0x1007ff8;
    *returnValue1 = 0xaabbccee;
#if 0
    while ( 1 )
        ;

    word_t some_reg;

    asm volatile (
        "csrr %0, sstatus    \n"
        "li %1, 10"
        :"=r" ( sstatus_reg ), "=r" ( some_reg )
    );
#endif
    init_cpu();

    ( void ) hardid;
    ( void ) dtb;
    printf ( "ELF-loader started on\n" );
    printf ( "sizeof ( word_t ): %d\n", sizeof ( word_t ) );


    interuptAcknowledge = 0;

    unsigned int cnt = 0;
    //unsigned int delay = 2500000;
    unsigned int delay = 250000;
    unsigned int interruptDelay = 0x1000000;
    while ( 1 )
    {
        for (unsigned int k = 0; k < delay; ++k )
            ;

#if defined(DECAY_DELAY)
        unsigned int newDelay = delay;
        newDelay = (9 * delay) / 10;
        delay = newDelay;
#endif

#if defined(DECAY_INTERRUPT)
        unsigned int newinterruptDelay;
        newinterruptDelay = (9 * interruptDelay) / 10;
        interruptDelay = newinterruptDelay;
#endif

        asm volatile (
            "csrr %0, sstatus"
            :"=r" ( sstatus_reg )
        );

        asm volatile (
            "csrr %0, sip"
            :"=r" ( sip_reg )
        );

        asm volatile (
            "csrr %0, sie"
            :"=r" ( sie_reg )
        );

        asm volatile (
            "csrr %0, scause"
            :"=r" ( scause_reg )
        );

        printf ( "ELF-loader started on: %d\n", cnt );
        printf ( "sstatus_reg: %x\n", sstatus_reg );
        printf ( "sie_reg: %x\n", sie_reg );
        printf ( "sip_reg: %x\n", sip_reg );
        printf ( "scause_reg: %x\n", scause_reg );
        printf ( "interuptAcknowledge: %x\n", interuptAcknowledge );
        printf ( "delay: %d\n", delay );
        printf ( "interruptDelay: %d\n", interruptDelay );
        printf ( "readSscratch: %d\n", readSscratch );

        printCycle ( get_cycles() );


        sbi_console_putchar ( 'a' );
        sbi_console_putchar ( '\n' );

        cnt++;

#if defined(KICK_INTERRUPT)
        if ( cnt == 10 )
        {
            printf ( "Enable interrupts\n" );
            init_cpu();

            set_sie_mask ( 0x20 );
            /* Allow interrupts in supervisor mode. */
            word_t allowSie = 0x02, tmp = 0;
            asm volatile ( "csrrs %0, sstatus, %1" : "=r" ( tmp ) : "rK" ( allowSie ) );



            initTimer(interruptDelay);

#if 0
            /* Trigger a software exception. */
            word_t value = 0x02;
            word_t temp;
            asm volatile ( "csrrs %0, sip, %1" : "=r" ( temp ) : "rK" ( value ) );
#endif
        }
#endif

        if ( interuptAcknowledge != 0 )
        {
            interuptAcknowledge = 0;
            initTimer(interruptDelay);
        }
#if 0
#endif
    }



    printf ( "  paddr=[%p..%p]\n", _start, _end - 1 );
    /* Unpack ELF images into memory. */
    
    (void)kernel_info;
    (void)user_info;
    (void)num_apps;
    //load_images(&kernel_info, &user_info, 1, &num_apps);

//#if 0    
    if (num_apps != 1) {
        printf("No user images loaded!\n");
        abort();
    }
//#endif

    map_kernel_window ( &kernel_info );

    printf ( "Jumping to kernel-image entry point...\n\n" );

    /*
       sfence.vma informs the processor that software has modified the
       page tables, so that it can guarantee that address translation
       will reflect the updates. (RISC-V book - 5.14)
    */
    asm volatile ( "sfence.vma" );

    asm volatile (
        "csrw sptbr, %0\n"
        :
        : "r"(vm_mode | (uintptr_t)l1pt >> RISCV_PGSHIFT)
        :
    );

    ((init_riscv_kernel_t)kernel_info.virt_entry)(user_info.phys_region_start,
                                                  user_info.phys_region_end, user_info.phys_virt_offset,
                                                  user_info.virt_entry);

    /* We should never get here. */
    printf("Kernel returned back to the elf-loader.\n");
}

#define LOAD  lw
#define _STRINGIFY(a) #a
#define STRINGIFY(a) _STRINGIFY(a)
#define LOAD_S STRINGIFY(LOAD)
//#define VISIBLE

//#define NORETURN
#define NORETURN __attribute__((__noreturn__))

#define NODE_UNLOCK_IF_HELD
static void c_exit_hook() {}

//static void UNREACHABLE() {}
#define UNREACHABLE() __builtin_unreachable()

/** DONT_TRANSLATE */
void VISIBLE NORETURN _elf_loader_restore_user_context ( void )
{
    word_t cur_thread_reg = ( word_t ) _elf_loader_sscratch;

    c_exit_hook();

    NODE_UNLOCK_IF_HELD;

    asm volatile (
        "mv t0, %[cur_thread]       \n"
        LOAD_S " ra, (0*%[REGSIZE])(t0)  \n"
        LOAD_S "  sp, (1*%[REGSIZE])(t0)  \n"
        LOAD_S "  gp, (2*%[REGSIZE])(t0)  \n"
        /* skip tp */
        /* skip x5/t0 */
        LOAD_S "  t2, (6*%[REGSIZE])(t0)  \n"
        LOAD_S "  s0, (7*%[REGSIZE])(t0)  \n"
        LOAD_S "  s1, (8*%[REGSIZE])(t0)  \n"
        LOAD_S "  a0, (9*%[REGSIZE])(t0) \n"
        LOAD_S "  a1, (10*%[REGSIZE])(t0) \n"
        LOAD_S "  a2, (11*%[REGSIZE])(t0) \n"
        LOAD_S "  a3, (12*%[REGSIZE])(t0) \n"
        LOAD_S "  a4, (13*%[REGSIZE])(t0) \n"
        LOAD_S "  a5, (14*%[REGSIZE])(t0) \n"
        LOAD_S "  a6, (15*%[REGSIZE])(t0) \n"
        LOAD_S "  a7, (16*%[REGSIZE])(t0) \n"
        LOAD_S "  s2, (17*%[REGSIZE])(t0) \n"
        LOAD_S "  s3, (18*%[REGSIZE])(t0) \n"
        LOAD_S "  s4, (19*%[REGSIZE])(t0) \n"
        LOAD_S "  s5, (20*%[REGSIZE])(t0) \n"
        LOAD_S "  s6, (21*%[REGSIZE])(t0) \n"
        LOAD_S "  s7, (22*%[REGSIZE])(t0) \n"
        LOAD_S "  s8, (23*%[REGSIZE])(t0) \n"
        LOAD_S "  s9, (24*%[REGSIZE])(t0) \n"
        LOAD_S "  s10, (25*%[REGSIZE])(t0)\n"
        LOAD_S "  s11, (26*%[REGSIZE])(t0)\n"
        LOAD_S "  t3, (27*%[REGSIZE])(t0) \n"
        LOAD_S "  t4, (28*%[REGSIZE])(t0) \n"
        LOAD_S "  t5, (29*%[REGSIZE])(t0) \n"
        LOAD_S "  t6, (30*%[REGSIZE])(t0) \n"
        /* Get next restored tp */
        LOAD_S "  t1, (3*%[REGSIZE])(t0)  \n"
        /* get restored tp */
        "add tp, t1, x0  \n"
        /* get sepc */
        LOAD_S "  t1, (34*%[REGSIZE])(t0)\n"
        "csrw sepc, t1  \n"

        /* Write back sscratch with cur_thread_reg to get it back on the next trap entry */
        "csrw sscratch, t0         \n"

        LOAD_S "  t1, (32*%[REGSIZE])(t0) \n"
        "csrw sstatus, t1\n"

        LOAD_S "  t1, (5*%[REGSIZE])(t0) \n"
        LOAD_S "  t0, (4*%[REGSIZE])(t0) \n"
        "sret"
        : /* no output */
        : [REGSIZE] "i" ( sizeof ( word_t ) ),
        [cur_thread] "r" ( cur_thread_reg )
        : "memory"
    );

    UNREACHABLE();
}

void NORETURN
_elf_loader_c_handle_interrupt ( void )
{
    printf ( "_elf_loader_c_handle_interrupt\n" );

    word_t scause_reg;
    asm volatile (
        "csrr %0, scause"
        :"=r" ( scause_reg )
    );
    printf ( "scause_reg: %x\n", scause_reg );

    _elf_loader_restore_user_context();

#if 0
    while ( 1 )
        ;
#endif

    UNREACHABLE();
}

void NORETURN
_elf_loader_c_handle_exception ( void )
{
#if 0
    printf ( "_elf_loader_c_handle_exception\n" );

    word_t scause_reg;
    asm volatile (
        "csrr %0, scause"
        :"=r" ( scause_reg )
    );
    printf ( "scause_reg: %x\n", scause_reg );
#endif
    clear_sip_mask ( 0x20 );

    _elf_loader_restore_user_context();

#if 0
    while ( 1 )
        ;
#endif

    UNREACHABLE();
}

void NORETURN
_elf_loader_c_handle_syscall ( word_t cptr, word_t msgInfo, word_t unused1, word_t unused2, word_t unused3, word_t unused4, word_t unused5, void *syscall )
{
    ( void ) cptr;
    ( void ) msgInfo;
    ( void ) unused1;
    ( void ) unused2;
    ( void ) unused3;
    ( void ) unused4;
    ( void ) unused5;
    ( void ) syscall;

    printf ( "_elf_loader_c_handle_syscall\n" );

    word_t scause_reg;
    asm volatile (
        "csrr %0, scause"
        :"=r" ( scause_reg )
    );
    printf ( "scause_reg: %x\n", scause_reg );

    _elf_loader_restore_user_context();

    UNREACHABLE();

#if 0
    while ( 1 )
        ;
#endif
}
