/* SPDX-License-Identifier: MIT
 *
 * hello32: the smallest possible flat 32-bit protected-mode guest,
 * linked at 0x100000 — the same address, entered with the same forged
 * segment/control-register state, as a Linux bzImage's startup_32
 * (vcpu_setup_linux32). If this payload runs, protected-mode *entry*
 * works and any Linux boot failure is further along; if this dies, we
 * have a ~10-instruction reproducer with no kernel involved.
 *
 * It then reports the CPUID a Linux guest would base boot decisions on:
 * max basic leaf, leaf 1 EDX/ECX, and leaf 7 EBX/ECX (ECX bit 16 =
 * LA57, which steers the decompression stub's 4- vs 5-level paging
 * choice).
 *
 * Talks straight at COM1's data port; each `out` is a KVM_EXIT_IO, so
 * even partial output is proof of executed instructions. Ends in HLT,
 * which (with no in-kernel irqchip) exits the run loop cleanly.
 */
    .code32
    .globl _start
_start:
    mov $0x100000, %esp         /* stack just below our own code       */

    mov $msg_hello, %esi
    call puts

    mov $msg_max, %esi
    call puts
    xor %eax, %eax
    cpuid
    call puthex                 /* max basic leaf                      */

    mov $msg_l1, %esi
    call puts
    mov $1, %eax
    xor %ecx, %ecx
    cpuid
    mov %edx, %ebp
    call puthex_ecx             /* leaf 1 ECX                          */
    mov $msg_sep, %esi
    call puts
    mov %ebp, %eax
    call puthex                 /* leaf 1 EDX                          */

    mov $msg_l7, %esi
    call puts
    mov $7, %eax
    xor %ecx, %ecx
    cpuid
    mov %ebx, %ebp
    call puthex_ecx             /* leaf 7 ECX (bit 16 = LA57)          */
    mov $msg_sep, %esi
    call puts
    mov %ebp, %eax
    call puthex                 /* leaf 7 EBX                          */

    /*
     * Control registers, as seen from inside the guest. The VMM set
     * CR0=0x60000011, CR3=0, CR4=0; under KVM shadow paging all three
     * differ from what the hardware really runs with, so reads are
     * (must be) intercepted and emulated. Anything else printed here
     * means the hypervisor is leaking host state.
     */
    mov $msg_cr, %esi
    call puts
    mov %cr0, %eax
    call puthex
    mov $msg_sep, %esi
    call puts
    mov %cr3, %eax
    call puthex
    mov $msg_sep, %esi
    call puts
    mov %cr4, %eax
    call puthex

    /*
     * Long-mode leg: recreate the exact environment where the Linux
     * decompression stub reads CR3, including the detail that matters
     * to a hypervisor: once the guest's CR0 equals the value the host
     * wants to run with (PG|WP set, CD|NW clear), KVM stops
     * intercepting CR0 reads/writes. CR3/CR4 reads must still be
     * intercepted. We read them from 32-bit compatibility mode and
     * from 64-bit mode separately, to see which decode path leaks.
     */
    mov $msg_efer, %esi
    call puts
    mov $0xc0000080, %ecx       /* EFER */
    rdmsr
    or $0x100, %eax             /* LME */
    wrmsr
    rdmsr
    call puthex                 /* expect 00000100 */

    /* identity-map the first 1 GiB: PML4@30000 -> PDPT@31000 -> PD@32000 */
    xor %eax, %eax
    mov $0x30000, %edi
    mov $(0x3000 / 4), %ecx
    rep stosl
    movl $0x31003, 0x30000
    movl $0x32003, 0x31000
    mov $0x32000, %edi
    mov $0x83, %eax             /* 2 MiB page, RW, present */
    mov $512, %ecx
1:  mov %eax, (%edi)
    add $0x200000, %eax
    add $8, %edi
    loop 1b

    mov $0x30000, %eax
    mov %eax, %cr3
    mov %cr4, %eax
    or $0x20, %eax              /* PAE */
    mov %eax, %cr4
    mov %cr0, %eax
    and $0x1fffffff, %eax       /* drop CD|NW: match KVM's effective CR0 */
    or $0x80010000, %eax        /* PG|WP: long mode goes live here */
    mov %eax, %cr0

    mov $msg_crc, %esi          /* compat mode: paging on, CS still 32-bit */
    call puts
    mov %cr0, %eax
    call puthex
    mov $msg_sep, %esi
    call puts
    mov %cr3, %eax
    call puthex
    mov $msg_sep, %esi
    call puts
    mov %cr4, %eax
    call puthex

    lgdt gdtr
    ljmpl $0x08, $long64

    .code64
long64:
    mov $msg_cr64, %esi
    call puts64
    mov %cr0, %rax
    call puthex64
    mov $msg_sep, %esi
    call puts64
    mov %cr3, %rax
    call puthex64
    mov $msg_sep, %esi
    call puts64
    mov %cr4, %rax
    call puthex64

    mov $msg_done, %esi
    call puts64
1:  hlt
    jmp 1b

puts64:
    mov $0x3f8, %dx
1:  lodsb
    test %al, %al
    je 2f
    out %al, %dx
    jmp 1b
2:  ret

puthex64:
    mov $0x3f8, %dx
    mov $16, %ecx
1:  rol $4, %rax
    mov %al, %bl
    and $0xf, %bl
    cmp $10, %bl
    jb 2f
    add $('a' - 10 - '0'), %bl
2:  add $'0', %bl
    xchg %rax, %rbx
    out %al, %dx
    xchg %rax, %rbx
    loop 1b
    ret

    .code32

/* print NUL-terminated string at %esi */
puts:
    mov $0x3f8, %dx
1:  lodsb
    test %al, %al
    je 2f
    out %al, %dx
    jmp 1b
2:  ret

/* print %ecx (via puthex_ecx) or %eax (via puthex) as 8 hex digits */
puthex_ecx:
    mov %ecx, %eax
puthex:
    mov $0x3f8, %dx
    mov $8, %ecx
1:  rol $4, %eax
    mov %al, %bl
    and $0xf, %bl
    cmp $10, %bl
    jb 2f
    add $('a' - 10 - '0'), %bl
2:  add $'0', %bl
    xchg %eax, %ebx
    out %al, %dx
    xchg %eax, %ebx
    loop 1b
    ret

msg_hello: .asciz "EMBER32: flat protected mode entered, fetch+DS+IO all work\r\n"
msg_max:   .asciz "cpuid.max="
msg_l1:    .asciz " cpuid.1.ecx/edx="
msg_l7:    .asciz " cpuid.7.ecx/ebx="
msg_sep:   .asciz "/"
msg_cr:    .asciz " cr0/cr3/cr4="
msg_efer:  .asciz "\r\nEMBER32: efer="
msg_crc:   .asciz " compat cr0/cr3/cr4="
msg_cr64:  .asciz "\r\nEMBER64: cr0/cr3/cr4="
msg_done:  .asciz "\r\nEMBER32: done\r\n"

    .balign 8
gdt:
    .quad 0
    .quad 0x00af9a000000ffff    /* 0x08: 64-bit code, L=1 */
    .quad 0x00cf92000000ffff    /* 0x10: data             */
gdt_end:
gdtr:
    .word gdt_end - gdt - 1
    .long gdt
