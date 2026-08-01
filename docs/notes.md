# embervisor: design notes

The long-form version of the README. What actually happens between
`./embervisor --kernel bzImage` and a shell prompt, in the order it
happens, plus the decisions and the dead ends.

## 1. The KVM contract

KVM's userspace API is three nested file descriptors:

```
/dev/kvm                    the system:  capabilities, one ioctl to make VMs
 └── VM fd                  the machine: memory slots, irqchip, devices
      └── vCPU fd           the CPU:     registers, and KVM_RUN
```

Everything embervisor does is ioctls on these three fds plus two mmaps.
There is no magic in the VMM. The magic is in the hardware (VT-x/SVM
root vs non-root mode) and in KVM's use of it. Userspace's job is to be
the machine's *world*: its memory map, its firmware (we replace it), and
every device it thinks it is talking to.

Guest RAM is one anonymous `mmap` in our address space, registered with
`KVM_SET_USER_MEMORY_REGION { guest_phys_addr = 0, userspace_addr = ptr,
memory_size = N }`. When the guest touches physical address `x`, the
CPU's second-level page tables (EPT on Intel, NPT on AMD) translate it
to our `ptr + x` at full speed, without exiting. This one-liner is doing
the job that shadow page tables did with enormous effort before ~2008,
and it is worth understanding why: with nested paging the hardware walks
*both* translations (guest virtual, then guest physical, then host
physical) in the TLB miss path, so memory virtualization costs nearly
nothing at steady state.

## 2. What a vCPU needs before it will run

`KVM_CREATE_VCPU` gives back a vCPU in x86 reset state: real mode,
`CS:IP` pointing at the reset vector, everything 16-bit. For the
bare-metal payload we barely touch it. Aim `CS:IP` at the load address
and go.

Linux is entered through the 32-bit boot protocol instead, which means
*we* put the vCPU in protected mode. KVM lets userspace set the segment
registers' hidden state directly (`KVM_SET_SREGS`), the part real
hardware only loads implicitly on a segment load: base, limit, type,
granularity. So there is no GDT in guest memory at entry. CS just *is* a
flat 4 GiB execute/read segment, because we said so. The catch is that
on Intel, VMENTER validates guest state architecturally. Three things
VMX insists on that cost me time:

- TR must hold a *busy* 32-bit TSS (type 11). Nothing will ever use it.
  It still has to be there.
- LDTR must be valid or explicitly unusable (`.unusable = 1`).
- RFLAGS bit 1 is reserved-set, so a zeroed rflags is invalid.

Get any of this wrong and you get `KVM_EXIT_FAIL_ENTRY` with an opaque
hardware reason code, which is the VMM equivalent of a linker error:
terse, but at least it fails *before* running garbage.

## 3. Loading a bzImage by hand

A bzImage is a real-mode setup stub stapled to a compressed
protected-mode kernel. Bootloaders historically jumped into the setup
stub in real mode. The modern contract (boot protocol >= 2.02, we
require >= 2.12) lets a loader skip it entirely:

1. Parse the setup header at offset `0x1f1`. Validate `0xAA55` at
   `0x1fe` and the `"HdrS"` magic. `setup_sects` tells you where the
   protected-mode image starts; copy that part to `0x100000`.
2. Build the zero page (`struct boot_params`) somewhere low (we use
   `0x10000`): copy the setup header into it, then fill in what the
   loader owes the kernel. That means `type_of_loader = 0xff`,
   `cmd_line_ptr`, `ramdisk_image`/`ramdisk_size` for the initrd (loaded
   as high as `initrd_addr_max` allows), and an e820 memory map.
3. Set `%esi` to the zero page, `%eip` to `0x100000`, and enter.

The e820 map deserves a note. It is how firmware describes physical
memory, and we *are* the firmware, so we hand out two ranges, 0 to 639
KiB and 1 MiB to RAM end, which leaves the traditional VGA/BIOS hole
alone. My first map said "0 to end, all usable" and the kernel happily
placed data over where the 8250's MMIO would have been if it had any.
The kernel trusts this map completely. Firmware bugs are kernel bugs
now.

The kernel's decompression stub (`startup_32` in
`arch/x86/boot/compressed/head_64.S`) needs only a handful of our
instructions to be valid: it immediately builds its own GDT and page
tables, switches to long mode, and decompresses the real kernel. Our job
ends ~200 instructions in. Everything after that is Linux's own boot.

## 4. Interrupts: the part that makes it a computer

Early versions ran with no irqchip. Fine for `hello.bin`, where every
`hlt` exits to userspace. Useless for Linux, which expects a PIC, an
IOAPIC, a LAPIC per CPU, and a timer.

`KVM_CREATE_IRQCHIP` + `KVM_CREATE_PIT2` put all four in the kernel. Two
consequences that shape the whole design:

- HLT stops exiting. With an in-kernel LAPIC, a halted vCPU blocks
  inside `KVM_RUN` until an interrupt is pending. An idle guest costs
  nothing and wakes precisely. (This is also why the flat/bare-metal
  mode deliberately skips the irqchip. There, HLT exiting *is* the clean
  shutdown signal.)
- Timer ticks never touch userspace. The PIT and LAPIC timer are
  emulated in-kernel, so the guest's scheduler tick just works.

Userspace's whole interrupt surface shrinks to one ioctl:
`KVM_IRQ_LINE(4, level)` when the UART's state changes. The kernel
routes it through the emulated PIC/IOAPIC into the LAPIC the way real
wiring would.

## 5. An honest 8250

The UART is where "minimal" stops being simple. A console you can type
into needs the receive path: stdin lands in the RX FIFO, IRQ 4 asserts,
the guest's ISR reads `RBR`, and the line drops when the FIFO drains.
embervisor runs a reader thread (stdin is blocking, and the vCPU thread
must never block on it), a mutex around register state, and real IIR
arbitration. RX-data beats TX-empty, reading IIR clears the TX-empty
cause, and the line level is recomputed after every register touch.

Details that turned out to matter:

- Linux probes before it trusts. The 8250 driver's autoconfig pokes the
  scratch register, flips loopback mode on via MCR and checks that MSR
  mirrors MCR's outputs, and writes FCR to see if IIR reports FIFOs.
  Emulate loopback or lose the console.
- `LSR.THRE|TEMT` is always set, because our TX is a `write(1, ...)` and
  the transmitter is therefore always empty. This makes the kernel's
  console write path (spin until THRE) free.
- Everything else on the ISA bus answers reads with `0xff`, like an open
  bus. The kernel's probes for PCI (`0xcf8`), a second UART, and an
  i8042 keyboard controller all see all-ones and conclude "absent",
  which is the graceful degradation a real motherboard gives you.

## 6. Shutdown without ACPI

No ACPI means no clean poweroff protocol. Instead: boot with `reboot=t
panic=-1`, and the kernel reboots via triple fault. It loads an empty
IDT and executes `int3`, the resulting cascade of faults has no handler,
and the CPU resets. Under KVM that surfaces as `KVM_EXIT_SHUTDOWN`,
which embervisor treats as "guest is done". It sounds like a hack. It is
actually the documented fallback path (`reboot=t` exists precisely for
machines with broken everything else), and it makes automated runs
terminate deterministically.

## 7. Testing a VMM with no virtualization hardware

Development happened in a cloud container with no `/dev/kvm`, which is a
problem for a program that is 90% ioctls on `/dev/kvm`. The rig in
`scripts/run-nested.sh`:

- L0: QEMU in pure TCG mode. Software emulation, needs nothing from the
  host, and models an AMD CPU *with SVM exposed*.
- L1: our kernel boots there, sees SVM, initializes `kvm_amd` against
  QEMU's emulation of it, and exports a fully real `/dev/kvm`.
- L2: embervisor runs in L1 unmodified and boots its guest.

The same binary, the same ioctl sequence, the same exit stream as bare
metal, only interpreted. `hello` completes in seconds. Full
Linux-under-embervisor-under-QEMU takes minutes, since every L2
instruction is ultimately TCG-interpreted through L1's emulated SVM
world switches, and it produces the most satisfying `uname -a` I have
ever read. The rig doubles as the test harness for the kindling modules,
which load into L1 directly.

## 8. Scope cuts, and what each would cost

| absent | what it would take |
|---|---|
| SMP | one thread + `KVM_CREATE_VCPU` per vCPU (the run loop is already per-vCPU); an MP table or ACPI MADT so the kernel finds the others; INIT/SIPI already emulated by KVM |
| virtio-blk/net | a PCI host bridge (intercept `0xcf8/0xcfc`), BAR allocation, then the virtio queue protocol; this is the next thing I want to build |
| ACPI | tables (FADT/DSDT) in guest memory + PM I/O ports; buys clean poweroff and SMP enumeration |
| 64-bit direct entry | build identity page tables, enter with paging on; skippable because the kernel's stub does it better |
| Migration/snapshots | `KVM_GET_*`/`KVM_SET_*` for every state class + dirty page tracking (`KVM_GET_DIRTY_LOG`), which is the part AHV's live migration does for a living |

## Reading that paid off

- `Documentation/virt/kvm/api.rst`, the whole userspace contract
- `Documentation/arch/x86/boot.rst`, definitive on the boot protocol
- kvmtool source, the proof that a real VMM fits in small C
- Serial Programming/8250 UART (the classic reference) for IIR/LSR
  semantics
- Intel SDM vol. 3C, "VM Entries", for every FAIL_ENTRY I earned
