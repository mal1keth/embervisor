/* SPDX-License-Identifier: MIT
 *
 * VM lifecycle: open /dev/kvm, create the VM, wire up guest RAM, and
 * (for Linux guests) ask KVM for its in-kernel interrupt controllers.
 *
 * KVM's model: /dev/kvm is a factory; KVM_CREATE_VM hands back a VM fd;
 * everything about one VM (memory slots, irqchip, vCPUs) hangs off that
 * fd. Guest physical memory is nothing magical. It is ordinary host
 * virtual memory that we mmap() and then describe to KVM with
 * KVM_SET_USER_MEMORY_REGION as "guest physical [0, ram_size) lives at
 * this host address". The hardware MMU's second-level page tables (EPT
 * on Intel, NPT on AMD) do the actual remapping at run time.
 */
#include "embervisor.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void vm_init(struct vm *vm, uint64_t ram_size)
{
    int api;

    memset(vm, 0, sizeof(*vm));
    vm->ram_size = ram_size;

    vm->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (vm->kvm_fd < 0)
        die("open /dev/kvm: %m\n"
            "  (KVM needs bare-metal Linux with VT-x/AMD-V, or nested "
            "virtualization.\n   In a container/VM without it, see "
            "scripts/run-nested.sh for a self-contained rig.)");

    /* The KVM userspace ABI is versioned once: 12, frozen forever. */
    api = ioctl(vm->kvm_fd, KVM_GET_API_VERSION, 0);
    if (api != KVM_API_VERSION)
        die("KVM api version %d, expected %d", api, KVM_API_VERSION);

    vm->fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 0);
    if (vm->fd < 0)
        die("KVM_CREATE_VM: %m");

    /*
     * Three pages of identity-map scratch space KVM's Intel code uses
     * to fake real mode on CPUs without "unrestricted guest" support.
     * Must not overlap guest RAM; the traditional spot just below 4G.
     */
    if (ioctl(vm->fd, KVM_SET_TSS_ADDR, 0xfffbd000UL) < 0)
        die("KVM_SET_TSS_ADDR: %m");

    vm->ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (vm->ram == MAP_FAILED)
        die("mmap %llu MiB of guest RAM: %m",
            (unsigned long long)(ram_size >> 20));

    struct kvm_userspace_memory_region region = {
        .slot            = 0,
        .flags           = 0,
        .guest_phys_addr = 0,
        .memory_size     = ram_size,
        .userspace_addr  = (uint64_t)vm->ram,
    };
    if (ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
        die("KVM_SET_USER_MEMORY_REGION: %m");

    info("VM created: %llu MiB RAM at one memslot",
         (unsigned long long)(ram_size >> 20));
}

/*
 * In-kernel PIC + IOAPIC + LAPIC, and the i8254 PIT. Must be called
 * before the first vCPU is created (the LAPIC is allocated per-vCPU at
 * KVM_CREATE_VCPU time). With these in the kernel, HLT no longer exits
 * to us. The vCPU blocks in-kernel until an interrupt is pending, and
 * timer ticks never leave the kernel at all. Userspace's only irq job
 * is pulsing lines for devices *we* emulate (the UART) via KVM_IRQ_LINE.
 */
void vm_create_irqchip(struct vm *vm)
{
    if (ioctl(vm->fd, KVM_CREATE_IRQCHIP, 0) < 0)
        die("KVM_CREATE_IRQCHIP: %m");

    struct kvm_pit_config pit = { .flags = 0 };
    if (ioctl(vm->fd, KVM_CREATE_PIT2, &pit) < 0)
        die("KVM_CREATE_PIT2: %m");

    vm->has_irqchip = true;
}

/* Bounds-checked guest-physical to host-virtual translation. */
void *vm_gpa(struct vm *vm, uint64_t gpa, uint64_t len)
{
    if (gpa >= vm->ram_size || len > vm->ram_size - gpa)
        die("guest physical [%#llx, +%#llx) outside %llu MiB of RAM",
            (unsigned long long)gpa, (unsigned long long)len,
            (unsigned long long)(vm->ram_size >> 20));
    return vm->ram + gpa;
}
