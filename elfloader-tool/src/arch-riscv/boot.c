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
#define PTE_LEVEL_BITS PT_LEVEL_2_BITS
#define PT_INDEX_BITS  10
#define PPN_HIGH       20
typedef uint32_t seL4_Word;
#else
#define PTE_LEVEL_BITS PT_LEVEL_1_BITS
#define PT_INDEX_BITS  9
#define PPN_HIGH       28
typedef uint64_t seL4_Word;
#endif

#define PTES_PER_PT BIT(PT_INDEX_BITS)

#define PTE_CREATE(PPN)          (unsigned long)(((uint32_t)PPN) | PTE_TYPE_SRWX | PTE_V)
#define PTE_CREATE_PPN(PT_BASE)  (unsigned long)(((PT_BASE) >> RISCV_PGSHIFT) << PTE_PPN0_SHIFT)
#define PTE_CREATE_NEXT(PT_BASE) (unsigned long)(PTE_CREATE_PPN(PT_BASE) | PTE_TYPE_TABLE | PTE_V)
#define PTE_CREATE_LEAF(PT_BASE) (unsigned long)(PTE_CREATE_PPN(PT_BASE) | PTE_TYPE_SRWX | PTE_V)

#define GET_PT_INDEX(addr, n) (((addr) >> (((PT_INDEX_BITS) * ((CONFIG_PT_LEVELS) - (n))) + RISCV_PGSHIFT)) % PTES_PER_PT)

#define VIRT_PHYS_ALIGNED(virt, phys, level_bits) (IS_ALIGNED((virt), (level_bits)) && IS_ALIGNED((phys), (level_bits)))

struct image_info kernel_info;
struct image_info user_info;

unsigned long l1pt[PTES_PER_PT] __attribute__ ( ( aligned ( 4096 ) ) );
unsigned long l2pt[PTES_PER_PT] __attribute__ ( ( aligned ( 4096 ) ) );

char elfloader_stack_alloc[BIT ( CONFIG_KERNEL_STACK_BITS )];

char *print_hex ( int width, uint64_t x )
{
    enum {NUM_BUF = 32, BUF_LENGTH = 64};
    static char rawBuf[NUM_BUF][64];
    static unsigned int index = 0;

    index = ( index + 1 ) % NUM_BUF;

    for ( int k = 0; k < BUF_LENGTH / 2; ++k )
    {
        rawBuf[index][k] = '0';
        rawBuf[index][k + ( BUF_LENGTH / 2 )] = 0;
    }

    sprintf ( rawBuf[index] + ( BUF_LENGTH / 2 ), "%lx", x );

    int len = 0;
    while ( rawBuf[index][len + ( BUF_LENGTH / 2 )] != 0 )
    {
        ++len;
    }

    return rawBuf[index] + ( BUF_LENGTH / 2 ) - width + len;
}

char *print_page_entry ( uint64_t x )
{
    static char buf[1024];

    uint64_t ppn2 = ( x >> ( 9 + 9 + 2 + 8 ) ) & ( ( 1ul << 26 ) - 1 );
    uint64_t ppn1 = ( x >> ( 0 + 9 + 2 + 8 ) ) & ( ( 1ul <<  9 ) - 1 );
    uint64_t ppn0 = ( x >> ( 0 + 0 + 2 + 8 ) ) & ( ( 1ul <<  9 ) - 1 );

    sprintf (
        buf,
        "PPN[2]:%s PPN[1]:%s PPN[0]:%s RSW:%s D:%s A:%s G:%s U:%s XWR:%s V:%s -> phys_addr:%s",
        print_hex ( 3, ( x >> ( 9 + 9 + 2 + 8 ) ) & ( ( 1ul << 26 ) - 1 ) ), /* PPN[2] */
        print_hex ( 3, ( x >> ( 0 + 9 + 2 + 8 ) ) & ( ( 1ul <<  9 ) - 1 ) ), /* PPN[1] */
        print_hex ( 3, ( x >> ( 0 + 0 + 2 + 8 ) ) & ( ( 1ul <<  9 ) - 1 ) ), /* PPN[0] */
        print_hex ( 1, ( x >> ( 0 + 0 + 0 + 8 ) ) & ( ( 1ul <<  2 ) - 1 ) ), /* RSW */
        print_hex ( 1, ( x >> ( 0 + 0 + 0 + 7 ) ) & ( ( 1ul <<  1 ) - 1 ) ), /* D */
        print_hex ( 1, ( x >> ( 0 + 0 + 0 + 6 ) ) & ( ( 1ul <<  1 ) - 1 ) ), /* A */
        print_hex ( 1, ( x >> ( 0 + 0 + 0 + 5 ) ) & ( ( 1ul <<  1 ) - 1 ) ), /* G */
        print_hex ( 1, ( x >> ( 0 + 0 + 0 + 4 ) ) & ( ( 1ul <<  1 ) - 1 ) ), /* U */
        print_hex ( 1, ( x >> ( 0 + 0 + 0 + 1 ) ) & ( ( 1ul <<  3 ) - 1 ) ), /* XWR */
        print_hex ( 1, ( x >> ( 0 + 0 + 0 + 0 ) ) & ( ( 1ul <<  1 ) - 1 ) ), /* V */
        print_hex ( 16, ( ( ( ppn2 << 18 ) + ( ppn1 << 9 ) + ppn0 ) << 12 ) ) );

    return buf;
}

void
map_kernel_window ( struct image_info *kernel_info )
{
    uint64_t i;
    uint32_t l1_index;

    // first create a complete set of 1-to-1 mappings for all of memory. this is a brute
    // force way to ensure this elfloader is mapped into the new address space
    for ( i = 0; i < PTES_PER_PT; i++ )
    {
        l1pt[i] = PTE_CREATE ( ( uint64_t ) ( i << PPN_HIGH ) );
        uint64_t x = PTE_CREATE ( ( uint64_t ) ( i << PPN_HIGH ) );
        printf (
            "l1pt[%s] = %s (%s)\n",
            print_hex ( 3, i ),
            print_hex ( 16, x ),
            print_page_entry ( x ) );

    }
    //Now create any neccessary entries for the kernel vaddr->paddr
    l1_index = GET_PT_INDEX ( kernel_info->virt_region_start, PT_LEVEL_1 );

    printf ( "\n" );
    printf ( "kernel_info->virt_region_start = %x\n", kernel_info->virt_region_start );
    printf ( "GET_PT_INDEX(kernel_info->virt_region_start, PT_LEVEL_1) = %x\n", l1_index );
    printf ( "\n" );

    /* Check if aligned to top level page table. For Sv32, 2MiB. For Sv39, 1GiB */
    if ( VIRT_PHYS_ALIGNED ( kernel_info->virt_region_start, kernel_info->phys_region_start, PTE_LEVEL_BITS ) )
    {
        for ( int page = 0; l1_index < PTES_PER_PT; l1_index++, page++ )
        {
            l1pt[l1_index] = PTE_CREATE_LEAF ( ( seL4_Word ) ( kernel_info->phys_region_start + ( page << PTE_LEVEL_BITS ) ) );
#if 0
            uint64_t x = PTE_CREATE_LEAF ( ( seL4_Word ) ( kernel_info->phys_region_start + ( page << PTE_LEVEL_BITS ) ) );
            printf (
                "l1pt[%d] = %lx (%s)\n",
                l1_index,
                x,
                print_page_entry ( x ) );
#endif
        }
    }
#if CONFIG_PT_LEVELS == 3
    else
        if ( VIRT_PHYS_ALIGNED ( kernel_info->virt_region_start, kernel_info->phys_region_start, PT_LEVEL_2_BITS ) )
        {
            uint32_t l2_index = GET_PT_INDEX ( kernel_info->virt_region_start, PT_LEVEL_2 );
            l1pt[l1_index] = PTE_CREATE_NEXT ( ( seL4_Word ) l2pt );

            uint64_t x = PTE_CREATE_NEXT ( ( seL4_Word ) l2pt );
            printf (
                "&l2pt = %lx\n",
                l2pt );
            printf ( "\n" );
            printf (
                "l1pt[%s] = %s (%s)\n",
                print_hex ( 3, l1_index ),
                print_hex ( 16, x ),
                print_page_entry ( x ) );
            printf ( "\n" );

            for ( int page = 0; l2_index < PTES_PER_PT; l2_index++, page++ )
            {
                l2pt[l2_index] = PTE_CREATE_LEAF ( kernel_info->phys_region_start + ( page << PT_LEVEL_2_BITS ) );
                uint64_t x = PTE_CREATE_LEAF ( kernel_info->phys_region_start + ( page << PT_LEVEL_2_BITS ) );
                printf (
                    "l2pt[%s] = %s (%s)\n",
                    print_hex ( 3, l2_index ),
                    print_hex ( 16, x ),
                    print_page_entry ( x ) );
            }
        }
#endif
        else
        {
            printf ( "Kernel not properly aligned\n" );
            abort();
        }
}

#if __riscv_xlen == 32
#define LW lw
#else
#define LW ld
#endif

#if CONFIG_PT_LEVELS == 2
uint64_t vm_mode = 0x1llu << 31;
#elif CONFIG_PT_LEVELS == 3
uint64_t vm_mode = 0x8llu << 60;
#elif CONFIG_PT_LEVELS == 4
uint64_t vm_mode = 0x9llu << 60;
#else
#error "Wrong PT level"
#endif

int num_apps = 0;
void main ( int hardid, unsigned long dtb )
{
    ( void ) hardid;
    printf ( "ELF-loader started on\n" );

    printf ( "  paddr=[%p..%p]\n", _start, _end - 1 );
    /* Unpack ELF images into memory. */
    load_images ( &kernel_info, &user_info, 1, &num_apps );
    if ( num_apps != 1 )
    {
        printf ( "No user images loaded!\n" );
        abort();
    }

    map_kernel_window ( &kernel_info );

    printf ( "Jumping to kernel-image entry point...\n\n" );

    asm volatile ( "sfence.vma" );

    asm volatile (
        "csrw sptbr, %0\n"
        :
        : "r" ( vm_mode | ( uintptr_t ) l1pt >> RISCV_PGSHIFT )
        :
    );

    ( ( init_riscv_kernel_t ) kernel_info.virt_entry ) ( user_info.phys_region_start,
            user_info.phys_region_end, user_info.phys_virt_offset,
            user_info.virt_entry, 0, dtb );

    /* We should never get here. */
    printf ( "Kernel returned back to the elf-loader.\n" );
}
