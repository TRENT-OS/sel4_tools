/*
 * Copyright 2018, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

#include <autoconf.h>
.extern main
.global _start
.extern __global_pointer$
.extern elfloader_stack_alloc
.extern interuptAcknowledge
.extern readSscratch

#define BIT(n) (1 << (n))
.section ".text.start"

_start:

/* BBL starts all HARTs and sends them to this entry point.
 * While we only support 1 core, we send remaining HARTs to a WFI loop.
 */
  li s0, CONFIG_FIRST_HART_ID
  bne  a0, s0, spin_hart

.option push
.option norelax
1:auipc gp, %pcrel_hi(__global_pointer$)
  addi  gp, gp, %pcrel_lo(1b)
.option pop

  la sp, (elfloader_stack_alloc + BIT(12))

  la s0, main
  jr s0
spin_hart:
  wfi
  j spin_hart


.align 8
.space 4096
_bootstack_top_core1:

.global _elf_loader_c_handle_syscall
.global _elf_loader_c_handle_exception
.global _elf_loader_c_handle_interrupt

#define STORE sw
#define REGBYTES 8

#undef CONFIG_WORD_SIZE
#define CONFIG_WORD_SIZE 32

.global _elf_loader_trap_entry

_elf_loader_trap_entry:

.option push
.option norelax

la t0, interuptAcknowledge
li t1, 0x01
sw t1, (0)(t0)

la t0, readSscratch
csrr t1, sscratch
sw t1, (0)(t0)

.option pop

  li t0, 0x20
  csrrc t0, sip, t0
  
  sret

/*
  Control and Status Register Read and Write:
  (1) save t0 in csr[sscratch = 0x0140] and (2) read csr[sscratch] (= the value before operation (1)) into t0
  t0 now contains the address (range) we use to save the registers
*/
  csrrw t0, sscratch, t0


  STORE ra, (0*REGBYTES)(t0)
  STORE sp, (1*REGBYTES)(t0)
  STORE gp, (2*REGBYTES)(t0)
  STORE tp, (3*REGBYTES)(t0)
  STORE t1, (5*REGBYTES)(t0)
  STORE t2, (6*REGBYTES)(t0)
  STORE s0, (7*REGBYTES)(t0)
  STORE s1, (8*REGBYTES)(t0)
  STORE a0, (9*REGBYTES)(t0)
  STORE a1, (10*REGBYTES)(t0)
  STORE a2, (11*REGBYTES)(t0)
  STORE a3, (12*REGBYTES)(t0)
  STORE a4, (13*REGBYTES)(t0)
  STORE a5, (14*REGBYTES)(t0)
  STORE a6, (15*REGBYTES)(t0)
  STORE a7, (16*REGBYTES)(t0)
  STORE s2, (17*REGBYTES)(t0)
  STORE s3, (18*REGBYTES)(t0)
  STORE s4, (19*REGBYTES)(t0)
  STORE s5, (20*REGBYTES)(t0)
  STORE s6, (21*REGBYTES)(t0)
  STORE s7, (22*REGBYTES)(t0)
  STORE s8, (23*REGBYTES)(t0)
  STORE s9, (24*REGBYTES)(t0)
  STORE s10, (25*REGBYTES)(t0)
  STORE s11, (26*REGBYTES)(t0)
  STORE t3, (27*REGBYTES)(t0)
  STORE t4, (28*REGBYTES)(t0)
  STORE t5, (29*REGBYTES)(t0)
  STORE t6, (30*REGBYTES)(t0)


/*
   Read t0 from csr[sscratch] and save it where the other registers have been saved
*/
  /* save t0 value */
  csrr  x1, sscratch
  STORE    x1, (4*REGBYTES)(t0)

/*
   Save the supervisor status register
   ("The sstatus register keeps track of the processor’s current operating state")
*/   
  csrr x1, sstatus
  STORE x1, (32*REGBYTES)(t0)

//OK
/*
  li t0, 0x20
  csrrc t0, sip, t0
  
  sret
*/
/*
   Save the supervisor cause register
   ("When a trap is taken into S-mode, scause is written with a code indicating the event that caused the trap.")
*/

  csrr s0, scause
  STORE s0, (31*REGBYTES)(t0)

/*
   Save the supervisor status register
   ("When a trap is taken into S-mode, sepc is written with the virtual address of the instruction that encountered the exception.")
*/   
  /* Save exception PC */
  csrr x1,  sepc
  STORE   x1, (33*REGBYTES)(t0)

/* 
   Initialize gp to our (trap handler = "kernel") data segment (= the middle of a 4K range which is intended to be addressed with gp)
   la is a pseudoinstruction for loading a 32 bit constant into a register
*/
#if 0   
  la gp, __global_pointer$
#endif

#if 0
  /* Load kernel's stack address */
  la sp, (kernel_stack_alloc + BIT(CONFIG_KERNEL_STACK_BITS))
#endif

/*
   Branch to interrupt if bit 31 of scause is 1 (= there was an interrupt)
*/
  /* Check if it's an interrupt */
  srli s2, s0, (CONFIG_WORD_SIZE - 1)
  li   s1, 0x1
  beq  s2, s1, interrupt

/*
   Branch to exception if it was not an interrupt and bit 3 was set = "Environment Call"
*/
  andi s0, s0, 0xf /* priv 1.10 defines up to 15 exceptions/interrupts */
  li   s4, 8       /* priv 1.10 has value 8 for ecall exception */
  bne  s0, s4, exception

syscall:
  /* Set the return address to sepc + 4 in the case of a system/environment call */
  addi x1, x1, 4
  /* Save NEXTPC */
  STORE   x1, (34*REGBYTES)(t0)
  j _elf_loader_restore_user_context
#if 0
  j _elf_loader_c_handle_syscall
#endif  

/* Not an interrupt or a syscall */
exception:
  /* Save NEXTPC */
  STORE   x1, (34*REGBYTES)(t0)
  j _elf_loader_restore_user_context
#if 0
  j _elf_loader_c_handle_exception
#endif  

interrupt:
  /* Save NEXTPC */
  STORE   x1, (34*REGBYTES)(t0)
  j _elf_loader_restore_user_context
#if 0
  j _elf_loader_c_handle_interrupt
#endif  
//#endif