/* SPDX-License-Identifier: MIT */
#include "embervisor.h"

#include <getopt.h>

#define DEFAULT_CMDLINE \
    "console=ttyS0 reboot=t panic=-1 i8042.nokbd i8042.noaux rdinit=/init"

static void usage(int code) __attribute__((noreturn));
static void usage(int code)
{
    fprintf(stderr,
"embervisor: a from-scratch KVM virtual machine monitor\n"
"\n"
"usage:\n"
"  embervisor --kernel bzImage [--initrd FILE] [--cmdline STR] [--mem MiB]\n"
"  embervisor --flat payload.bin [--mem MiB]\n"
"\n"
"options:\n"
"  -k, --kernel FILE    boot a Linux bzImage (32-bit boot protocol)\n"
"  -i, --initrd FILE    initramfs for the kernel\n"
"  -c, --cmdline STR    kernel command line\n"
"                       (default: \"%s\")\n"
"  -f, --flat FILE      run a flat real-mode binary at %#llx instead\n"
"  -m, --mem MIB        guest RAM in MiB (default 256)\n"
"  -v, --verbose        chatty diagnostics on stderr\n"
"  -h, --help           this text\n"
"\n"
"console: the guest's COM1 is your terminal. Ctrl-A x quits,\n"
"         Ctrl-A Ctrl-A sends a literal Ctrl-A.\n",
    DEFAULT_CMDLINE, (unsigned long long)EMBER_FLAT_LOAD_ADDR);
    exit(code);
}

int main(int argc, char **argv)
{
    const char *kernel = NULL, *initrd = NULL, *flat = NULL;
    const char *cmdline = DEFAULT_CMDLINE;
    uint64_t mem_mib = 256;
    struct vm vm;
    struct vcpu vcpu;
    struct serial *serial;
    int c;

    static const struct option opts[] = {
        { "kernel",  required_argument, NULL, 'k' },
        { "initrd",  required_argument, NULL, 'i' },
        { "cmdline", required_argument, NULL, 'c' },
        { "flat",    required_argument, NULL, 'f' },
        { "mem",     required_argument, NULL, 'm' },
        { "verbose", no_argument,       NULL, 'v' },
        { "help",    no_argument,       NULL, 'h' },
        { 0 },
    };

    while ((c = getopt_long(argc, argv, "k:i:c:f:m:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'k': kernel = optarg; break;
        case 'i': initrd = optarg; break;
        case 'c': cmdline = optarg; break;
        case 'f': flat = optarg; break;
        case 'm': mem_mib = strtoull(optarg, NULL, 0); break;
        case 'v': ember_verbose = true; break;
        case 'h': usage(0);
        default:  usage(1);
        }
    }
    if (!!kernel == !!flat) {
        fprintf(stderr, "embervisor: need exactly one of --kernel/--flat\n\n");
        usage(1);
    }
    if (mem_mib < 16 || (kernel && mem_mib < 64))
        die("--mem too small (min 16 MiB flat, 64 MiB for Linux)");

    vm_init(&vm, mem_mib << 20);

    if (kernel) {
        /*
         * Interrupt controllers must exist before the vCPU: KVM
         * allocates the vCPU's local APIC at KVM_CREATE_VCPU time.
         */
        vm_create_irqchip(&vm);
        vcpu_init(&vcpu, &vm, 0);
        vcpu_set_cpuid(&vcpu);

        uint64_t entry = load_bzimage(&vm, kernel, initrd, cmdline);
        vcpu_setup_linux32(&vcpu, entry, EMBER_BOOT_PARAMS_ADDR);
    } else {
        vcpu_init(&vcpu, &vm, 0);
        vcpu_set_cpuid(&vcpu);

        load_flat(&vm, flat, EMBER_FLAT_LOAD_ADDR);
        vcpu_setup_realmode(&vcpu, EMBER_FLAT_LOAD_ADDR);
    }

    serial = serial_create(&vm);
    return vcpu_loop(&vcpu, serial);
}
