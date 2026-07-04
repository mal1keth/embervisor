/* SPDX-License-Identifier: MIT
 *
 * embervisor: a from-scratch KVM virtual machine monitor.
 *
 * One VM, one vCPU, guest RAM mapped with a single memslot, an emulated
 * 8250 UART on the legacy COM1 ports, and just enough of the Linux x86
 * boot protocol to take a stock bzImage from cold reset to a shell.
 */
#pragma once

#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/kvm.h>

/* ---- guest physical memory layout (see docs/boot-protocol.md) ---- */
#define EMBER_FLAT_LOAD_ADDR   0x1000ULL    /* flat real-mode payloads     */
#define EMBER_BOOT_PARAMS_ADDR 0x10000ULL   /* "zero page" (boot_params)   */
#define EMBER_CMDLINE_ADDR     0x20000ULL   /* kernel command line         */
#define EMBER_KERNEL_LOAD_ADDR 0x100000ULL  /* protected-mode kernel, 1MiB */

#define EMBER_SERIAL_IRQ 4                  /* COM1 -> ISA IRQ 4           */

struct vm {
    int kvm_fd;                 /* /dev/kvm                                */
    int fd;                     /* the VM (KVM_CREATE_VM)                  */
    uint64_t ram_size;
    uint8_t *ram;               /* host mapping of guest [0, ram_size)     */
    bool has_irqchip;           /* in-kernel PIC/IOAPIC/LAPIC + PIT        */
};

struct vcpu {
    int fd;
    struct kvm_run *run;        /* shared run structure (mmap'd)           */
    size_t run_size;
    struct vm *vm;
};

extern bool ember_verbose;

struct serial;

/* util.c */
void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* vm.c */
void vm_init(struct vm *vm, uint64_t ram_size);
void vm_create_irqchip(struct vm *vm);
void *vm_gpa(struct vm *vm, uint64_t gpa, uint64_t len); /* checked translate */

/* vcpu.c */
void vcpu_init(struct vcpu *vcpu, struct vm *vm, int id);
void vcpu_set_cpuid(struct vcpu *vcpu);
void vcpu_setup_realmode(struct vcpu *vcpu, uint64_t rip);
void vcpu_setup_linux32(struct vcpu *vcpu, uint64_t rip, uint64_t boot_params);
int  vcpu_loop(struct vcpu *vcpu, struct serial *serial);

/* serial.c */
struct serial *serial_create(struct vm *vm);
bool serial_owns_port(uint16_t port);
void serial_io(struct serial *s, uint16_t port, bool is_write,
               uint8_t *data, uint32_t size);

/* loader.c */
void load_flat(struct vm *vm, const char *path, uint64_t addr);
uint64_t load_bzimage(struct vm *vm, const char *kernel_path,
                      const char *initrd_path, const char *cmdline);
