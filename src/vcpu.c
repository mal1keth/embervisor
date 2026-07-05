/* SPDX-License-Identifier: MIT
 *
 * vCPU setup and the run loop.
 *
 * A KVM vCPU is a file descriptor plus a shared, mmap'd `struct kvm_run`.
 * KVM_RUN enters the guest and returns only when the guest does something
 * hardware cannot handle alone: a port I/O access, an unmapped MMIO
 * access, a halt with no in-kernel irqchip, a triple fault. The exit
 * reason and its payload are in kvm_run when the ioctl returns; we
 * service it and re-enter. That loop *is* the virtual machine monitor.
 *
 * Register state is set with KVM_SET_REGS/KVM_SET_SREGS. The segment
 * "hidden parts" (base/limit/type that real hardware caches on segment
 * load) are set directly, which is how we can drop a vCPU straight into
 * flat 32-bit protected mode without ever writing a GDT into guest RAM.
 */
#include "embervisor.h"

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

void vcpu_init(struct vcpu *vcpu, struct vm *vm, int id)
{
    long sz;

    memset(vcpu, 0, sizeof(*vcpu));
    vcpu->vm = vm;

    vcpu->fd = ioctl(vm->fd, KVM_CREATE_VCPU, id);
    if (vcpu->fd < 0)
        die("KVM_CREATE_VCPU: %m");

    sz = ioctl(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (sz < 0)
        die("KVM_GET_VCPU_MMAP_SIZE: %m");
    vcpu->run_size = (size_t)sz;

    vcpu->run = mmap(NULL, vcpu->run_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, vcpu->fd, 0);
    if (vcpu->run == MAP_FAILED)
        die("mmap kvm_run: %m");
}

/*
 * Give the guest the CPUID the host supports. Without this the guest
 * sees a CPU that answers every CPUID leaf with zeroes. Linux will
 * still limp along surprisingly far, but feature detection (TSC,
 * APIC, ...) does much better with the truth.
 */
void vcpu_set_cpuid(struct vcpu *vcpu)
{
    struct kvm_cpuid2 *cpuid;
    int nent = 128;

    cpuid = calloc(1, sizeof(*cpuid) + nent * sizeof(cpuid->entries[0]));
    if (!cpuid)
        die("calloc cpuid");
    cpuid->nent = nent;

    if (ioctl(vcpu->vm->kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) < 0)
        die("KVM_GET_SUPPORTED_CPUID: %m");
    if (ioctl(vcpu->fd, KVM_SET_CPUID2, cpuid) < 0)
        die("KVM_SET_CPUID2: %m");

    free(cpuid);
}

/* 16-bit real mode, flat, IP at `rip`. For bare-metal test payloads. */
void vcpu_setup_realmode(struct vcpu *vcpu, uint64_t rip)
{
    struct kvm_sregs sregs;
    struct kvm_regs regs;

    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
        die("KVM_GET_SREGS: %m");
    /* Reset state is already real mode; just aim CS:IP at the payload. */
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
        die("KVM_SET_SREGS: %m");

    memset(&regs, 0, sizeof(regs));
    regs.rflags = 0x2;          /* bit 1 is reserved-set, everything else off */
    regs.rip = rip;
    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0)
        die("KVM_SET_REGS: %m");
    info("vcpu: real mode, CS:IP = 0:%#llx", (unsigned long long)rip);
}

static void flat_seg(struct kvm_segment *seg, uint16_t sel, uint8_t type,
                     uint8_t s)
{
    *seg = (struct kvm_segment){
        .base     = 0,
        .limit    = 0xffffffffu,
        .selector = sel,
        .type     = type,
        .present  = 1,
        .dpl      = 0,
        .db       = 1,          /* 32-bit */
        .s        = s,          /* code/data vs system */
        .l        = 0,
        .g        = 1,          /* 4 KiB granularity */
    };
}

/*
 * Enter the kernel via the x86 32-bit boot protocol
 * (Documentation/arch/x86/boot.rst): flat protected mode, paging off,
 * %esi = physical address of boot_params, %eip = the protected-mode
 * kernel entry. The decompression stub builds its own GDT, page tables
 * and stack, so the state below only has to survive a handful of
 * instructions, but it has to be architecturally *valid*, or VMX will
 * refuse to VMENTER (KVM_EXIT_FAIL_ENTRY, invalid guest state).
 */
void vcpu_setup_linux32(struct vcpu *vcpu, uint64_t rip, uint64_t boot_params)
{
    struct kvm_sregs sregs;
    struct kvm_regs regs;

    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
        die("KVM_GET_SREGS: %m");

    flat_seg(&sregs.cs, 0x10, 0xb /* exec/read, accessed */, 1);
    flat_seg(&sregs.ds, 0x18, 0x3 /* read/write, accessed */, 1);
    sregs.es = sregs.fs = sregs.gs = sregs.ss = sregs.ds;

    /* VMX demands a busy 32-bit TSS in TR; nobody will ever use it. */
    sregs.tr = (struct kvm_segment){
        .base = 0, .limit = 0xffff, .selector = 0x20,
        .type = 0xb, .present = 1, .s = 0,
    };
    sregs.ldt = (struct kvm_segment){ .unusable = 1 };

    sregs.cr0 |= 0x1;           /* PE: protected mode on, paging still off */
    sregs.cr3 = 0;
    sregs.cr4 = 0;
    sregs.efer = 0;

    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
        die("KVM_SET_SREGS: %m");

    memset(&regs, 0, sizeof(regs));
    regs.rflags = 0x2;
    regs.rip = rip;             /* 0x100000: startup_32 in the setup stub */
    regs.rsi = boot_params;     /* the one true boot protocol register    */
    regs.rsp = 0x9f000;         /* scratch; the stub switches stacks fast */
    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0)
        die("KVM_SET_REGS: %m");
}

/* Ports we knowingly ignore instead of warning about. */
static bool port_is_boring(uint16_t port)
{
    switch (port) {
    case 0x60: case 0x64:               /* i8042 keyboard controller */
    case 0x70: case 0x71:               /* CMOS/RTC                  */
    case 0x80:                          /* POST/delay port           */
    case 0x92:                          /* fast A20 gate             */
    case 0x2f8 ... 0x2ff:               /* COM2 probe                */
    case 0x3e8 ... 0x3ef:               /* COM3 probe                */
    case 0x2e8 ... 0x2ef:               /* COM4 probe                */
    case 0xcf8 ... 0xcff:               /* PCI config space          */
    case 0x4d0: case 0x4d1:             /* ELCR (PIC edge/level)     */
        return true;
    default:
        return false;
    }
}

static void handle_io(struct vcpu *vcpu, struct serial *serial)
{
    struct kvm_run *run = vcpu->run;
    uint8_t *data = (uint8_t *)run + run->io.data_offset;
    bool is_write = run->io.direction == KVM_EXIT_IO_OUT;
    uint32_t i;

    for (i = 0; i < run->io.count; i++, data += run->io.size) {
        if (serial && serial_owns_port(run->io.port)) {
            serial_io(serial, run->io.port, is_write, data, run->io.size);
            continue;
        }
        /*
         * Everything else: reads float high like an empty ISA bus
         * (0xff), writes vanish. This is exactly why the kernel's
         * probe-by-poking drivers (PCI, i8042, extra UARTs) conclude
         * "nothing there" and move on gracefully.
         */
        if (!is_write)
            memset(data, 0xff, run->io.size);
        else if (!port_is_boring(run->io.port))
            info("ignored write to port %#x (%u bytes)",
                 run->io.port, run->io.size);
    }
}

int vcpu_loop(struct vcpu *vcpu, struct serial *serial)
{
    struct kvm_run *run = vcpu->run;
    unsigned long exits = 0;

    info("entering run loop");
    for (;;) {
        if (ioctl(vcpu->fd, KVM_RUN, 0) < 0) {
            if (errno == EINTR)
                continue;
            die("KVM_RUN: %m");
        }
        if (++exits <= 20)
            info("exit %lu: reason %u", exits, run->exit_reason);

        switch (run->exit_reason) {
        case KVM_EXIT_IO:
            handle_io(vcpu, serial);
            break;

        case KVM_EXIT_MMIO:
            /* Same philosophy as unknown ports: reads float, writes drop. */
            if (!run->mmio.is_write)
                memset(run->mmio.data, 0xff, run->mmio.len);
            info("stray mmio %s at %#llx len %u",
                 run->mmio.is_write ? "write" : "read",
                 (unsigned long long)run->mmio.phys_addr, run->mmio.len);
            break;

        case KVM_EXIT_HLT:
            /* Only reachable without the in-kernel irqchip (flat mode). */
            fflush(stdout);
            fprintf(stderr, "\nembervisor: guest executed HLT, done\n");
            return 0;

        case KVM_EXIT_SHUTDOWN:
            /*
             * Triple fault. With `reboot=t panic=-1` this is how the
             * guest kernel reboots/poweroffs, so treat it as shutdown.
             */
            fflush(stdout);
            fprintf(stderr, "\nembervisor: guest reset (triple fault), "
                            "shutting down\n");
            return 0;

        case KVM_EXIT_FAIL_ENTRY:
            die("VMENTER failed, hw reason %#llx (invalid guest state?)",
                (unsigned long long)
                run->fail_entry.hardware_entry_failure_reason);

        case KVM_EXIT_INTERNAL_ERROR:
            die("KVM internal error, suberror %u", run->internal.suberror);

        default:
            die("unhandled exit reason %u", run->exit_reason);
        }
    }
}
