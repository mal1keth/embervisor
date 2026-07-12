/* SPDX-License-Identifier: MIT
 *
 * Guest image loading: flat binaries for bare-metal payloads, and the
 * Linux/x86 boot protocol (Documentation/arch/x86/boot.rst) for booting
 * a stock bzImage.
 *
 * A bzImage is two things glued together: a legacy real-mode setup blob
 * (`setup_sects` x 512 bytes, plus the boot sector) that we entirely
 * skip, and the protected-mode kernel proper, which we copy to 1 MiB.
 * The contract for skipping the real-mode part is the "32-bit boot
 * protocol": build the `boot_params` page ("zero page") ourselves,
 * with the setup header copied from the file, plus command line,
 * initrd location, and an e820 memory map. Then jump to 0x100000 with
 * %esi pointing at it. The kernel's decompression stub does the rest.
 */
#include "embervisor.h"

#include <asm/bootparam.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define E820_TYPE_RAM 1

/* Offsets/flags from the boot protocol spec. */
#define SETUP_HEADER_OFF   0x1f1
#define SETUP_MAGIC        0x53726448   /* "HdrS", little-endian        */
#define BOOT_FLAG          0xaa55
#define LOADFLAGS_HIGH     0x01         /* protected-mode code at 1 MiB */
#define LOADFLAGS_HEAP     0x80         /* CAN_USE_HEAP                 */

static uint8_t *read_file(const char *path, size_t *size_out)
{
    struct stat st;
    uint8_t *buf;
    ssize_t n;
    size_t off = 0;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        die("open %s: %m", path);
    if (fstat(fd, &st) < 0)
        die("fstat %s: %m", path);

    buf = malloc(st.st_size);
    if (!buf)
        die("malloc %lld bytes for %s", (long long)st.st_size, path);

    while (off < (size_t)st.st_size) {
        n = read(fd, buf + off, st.st_size - off);
        if (n < 0)
            die("read %s: %m", path);
        if (n == 0)
            die("read %s: unexpected EOF", path);
        off += n;
    }
    close(fd);

    *size_out = st.st_size;
    return buf;
}

void load_flat(struct vm *vm, const char *path, uint64_t addr)
{
    size_t size;
    uint8_t *buf = read_file(path, &size);

    memcpy(vm_gpa(vm, addr, size), buf, size);
    free(buf);
    info("flat payload %s: %zu bytes at %#llx", path, size,
         (unsigned long long)addr);
}

uint64_t load_bzimage(struct vm *vm, const char *kernel_path,
                      const char *initrd_path, const char *cmdline)
{
    size_t kernel_size, setup_size, prot_size, header_len, cmdline_len;
    uint8_t *image = read_file(kernel_path, &kernel_size);
    struct boot_params *bp;
    unsigned setup_sects;

    if (kernel_size < 8192)
        die("%s: too small to be a bzImage", kernel_path);
    if (*(uint16_t *)(image + 0x1fe) != BOOT_FLAG ||
        *(uint32_t *)(image + 0x202) != SETUP_MAGIC)
        die("%s: bad boot signature, not a bzImage?", kernel_path);
    if (*(uint16_t *)(image + 0x206) < 0x020c)
        die("%s: boot protocol < 2.12, kernel too old for this loader",
            kernel_path);

    /* 1. The protected-mode kernel goes to 1 MiB. */
    setup_sects = image[SETUP_HEADER_OFF];
    if (setup_sects == 0)
        setup_sects = 4;        /* historical quirk mandated by the spec */
    setup_size = (setup_sects + 1) * 512;
    if (setup_size >= kernel_size)
        die("%s: setup_sects points past end of file", kernel_path);
    prot_size = kernel_size - setup_size;

    memcpy(vm_gpa(vm, EMBER_KERNEL_LOAD_ADDR, prot_size),
           image + setup_size, prot_size);

    /* 2. Build the zero page: copy the setup header, then fill it in. */
    bp = vm_gpa(vm, EMBER_BOOT_PARAMS_ADDR, sizeof(*bp));
    memset(bp, 0, 4096);

    header_len = 0x202 + image[0x201] - SETUP_HEADER_OFF;
    memcpy((uint8_t *)bp + SETUP_HEADER_OFF, image + SETUP_HEADER_OFF,
           header_len);

    bp->hdr.type_of_loader = 0xff;              /* "undefined" loader ID  */
    bp->hdr.loadflags |= LOADFLAGS_HIGH | LOADFLAGS_HEAP;
    bp->hdr.heap_end_ptr = 0xfe00 - 0x200;      /* real-mode heap; unused */

    /* 3. Command line. */
    cmdline_len = strlen(cmdline) + 1;
    if (cmdline_len > bp->hdr.cmdline_size)
        die("command line longer than kernel supports (%u)",
            bp->hdr.cmdline_size);
    memcpy(vm_gpa(vm, EMBER_CMDLINE_ADDR, cmdline_len), cmdline, cmdline_len);
    bp->hdr.cmd_line_ptr = EMBER_CMDLINE_ADDR;

    /* 4. Initrd, loaded as high as the kernel allows. */
    if (initrd_path) {
        size_t initrd_size;
        uint8_t *initrd = read_file(initrd_path, &initrd_size);
        uint64_t ceiling = bp->hdr.initrd_addr_max;
        uint64_t addr;

        if (ceiling > vm->ram_size - 1)
            ceiling = vm->ram_size - 1;
        if (initrd_size > ceiling)
            die("initrd (%zu bytes) doesn't fit below %#llx",
                initrd_size, (unsigned long long)ceiling);
        addr = (ceiling + 1 - initrd_size) & ~0xfffULL;
        if (addr < EMBER_KERNEL_LOAD_ADDR + prot_size)
            die("initrd would overlap the kernel; give the VM more RAM");

        memcpy(vm_gpa(vm, addr, initrd_size), initrd, initrd_size);
        bp->hdr.ramdisk_image = addr;
        bp->hdr.ramdisk_size = initrd_size;
        free(initrd);
        info("initrd %s: %zu bytes at %#llx", initrd_path, initrd_size,
             (unsigned long long)addr);
    }

    /*
     * 5. The e820 map: how firmware traditionally tells the kernel
     * what physical memory exists. Two usable ranges: conventional
     * memory under 640K, and everything from 1 MiB up. The gap is the
     * legacy VGA/BIOS hole a PC kernel expects to see.
     */
    bp->e820_entries = 2;
    bp->e820_table[0] = (struct boot_e820_entry){
        .addr = 0, .size = 0x9fc00, .type = E820_TYPE_RAM,
    };
    bp->e820_table[1] = (struct boot_e820_entry){
        .addr = EMBER_KERNEL_LOAD_ADDR,
        .size = vm->ram_size - EMBER_KERNEL_LOAD_ADDR,
        .type = E820_TYPE_RAM,
    };

    info("bzImage %s: boot protocol %u.%02u, %zu KiB protected-mode code "
         "at %#llx", kernel_path, bp->hdr.version >> 8,
         bp->hdr.version & 0xff, prot_size / 1024,
         (unsigned long long)EMBER_KERNEL_LOAD_ADDR);

    free(image);
    return EMBER_KERNEL_LOAD_ADDR;
}
