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

## 8. A QEMU TCG-SVM bug, and how the rig cornered it

The nested rig earns its keep here. embervisor boots Linux fine when the
rig runs on **QEMU 8.2** (Ubuntu 24.04). On **QEMU 10.0** (Debian 13) the
*same* embervisor binary and *same* bzImage triple-fault on the **first
VMRUN** of the protected-mode guest: the very first `KVM_RUN` returns
`KVM_EXIT_SHUTDOWN`, zero serial output. Real-mode (`hello`) guests work
on both. This is the story of running that down; the short version is
that it is a bug in QEMU's own instruction decoder, not in embervisor,
and there is a one-line fix.

**Where it dies.** A `KVM_GET_REGS`/`KVM_GET_SREGS` in the
`KVM_EXIT_SHUTDOWN` path (`vcpu_dump_state` in `vcpu.c`) puts the corpse
on the table: `RIP = CR2 = 0x…b453`, i.e. an *instruction-fetch* page
fault — the faulting address is the instruction pointer itself — with
`CR3` equal to the kernel's `_pgtable`. Matching `RIP` against the
decompression stub's `vmlinux` symbols lands inside `memset`, called
(per the return address on the guest stack) from the *else* branch of
`initialize_identity_maps()`. That branch runs `memset(_pgtable, 0,
BOOT_PGT_SIZE)` — and `CR3` already points at `_pgtable`. The stub is
zeroing its own live top-level page table, so the next instruction fetch
has no mapping: #PF → #DF → shutdown.

That branch is guarded precisely to avoid this. The stub reads its own
`CR3` (`read_cr3_pa()`) and, if it already equals `_pgtable`, takes the
*append* branch instead of the *overwrite* branch. So the guest must be
**misreading CR3**.

**Isolating the entry state.** Before blaming CR3, rule out our forged
protected-mode state. The `--flat32` mode (see `main.c`) enters a
dozen-instruction payload at 1 MiB with the *exact* segment and
control-register state `vcpu_setup_linux32` hands the kernel, climbs it
into 64-bit long mode under its own page tables, and prints what it
sees. It boots to `HLT` cleanly on QEMU 10 — so entry-state handling is
fine, and the fault is something the *kernel stub* does that the payload
doesn't. But the same payload also printed the tell: it loaded
`CR3 = 0x30000`, then read it straight back as `0x024c8000`, and read
`CR4` back as `0x60` when it had written `0x20`. The reads were
returning host/shadow state, not the guest's.

**The bug.** Built QEMU 10.0.0 from source with two `fprintf`s in
`target/i386/tcg/system/svm_helper.c`: one per VMRUN dumping the loaded
intercept bitmaps, one in `cpu_svm_check_intercept_param` logging every
CR/DR intercept check. The failing boot's last check before death is
unambiguous:

```
chk type=000 eip=…d990 (initialize_identity_maps) cr_read=0018 hit=0
```

`type=000` is `SVM_EXIT_READ_CR0`. But the instruction is `MOV %cr3,%rax`
— it should be checked as `SVM_EXIT_READ_CR3` (`0x003`). Because it is
mistyped as a CR0 read, and KVM has by then **cleared** the CR0-read
intercept (`cr_read = 0x0018`: bits 3 and 4 set for CR3/CR4, bit 0
clear), the check misses, no #VMEXIT happens, and QEMU emulates the read
locally — handing the guest the shadow CR3.

Why CR0 clear but CR3 set: with NPT off, `kvm_amd` runs shadow paging and
uses the CR3 read/write intercepts to virtualize CR3, while its
"selective CR0 write" optimization *drops* the CR0 intercepts once the
guest's CR0 matches the host's. That steady state — CR0 reads free, CR3
reads trapped — is exactly what a booting kernel reaches, and exactly
what the mistyped check breaks.

The defect is in QEMU's new x86 decoder,
`target/i386/tcg/decode-new.c.inc`, and it is the *second* of two guards
that both confuse the **value** of the SVM exit code with a "has an
intercept" **flag**. Both stem from `SVM_EXIT_READ_CR0` being numerically
`0x000`.

The first guard — *whether to emit an intercept check at all* — was
already fixed upstream. Paolo Bonzini's "target/i386: fix processing of
intercept 0 (read CR0)" (qemu-devel, June 2024) introduced the
`has_intercept` flag precisely because exit code `0` was read as "no
intercept", and changed the dispatch site to `if (decode.e.has_intercept
&& GUEST(s))`. That shipped in QEMU 9.1.0, so it is present in the whole
10.0 line — this is *not* that bug, and credit for spotting the pattern
is his.

The guard that bites here is a different line: the one in `decode_op`
(the `X86_TYPE_C` / `X86_TYPE_D` cases) that adds the register number so
a CR3 read becomes `READ_CR3` instead of `READ_CR0`:

```c
if (decode->e.intercept) {       /* still the value, not the flag */
    decode->e.intercept += op->n;
}
```

`svm(READ_CR0)` sets `.intercept = 0` and `.has_intercept = true`. This
guard still tests the value, so for a CR *read* (base `0`) it is false
and `op->n` is never added — CR3/CR4/CR8 reads keep exit code `0` and are
checked against the CR0-read intercept bit. Writes (`WRITE_CR0 = 0x010`)
and DR access (`READ_DR0 = 0x020`) have non-zero bases, so they were
unaffected. The fix mirrors Bonzini's — test the flag, not the value:

```c
if (decode->e.has_intercept) {
    decode->e.intercept += op->n;
}
```

Rebuilt with that one line, the rig boots L2 Linux all the way to
userspace on QEMU 10.0.0 (`== L2 probe: Linux is alive under embervisor
==`, `model name: Hammer`, then a clean `reboot=t` exit); unpatched
10.0.0 triple-faults on the same input. QEMU 8.2 predates this decoder
entirely, which is why 8.2 worked.

**Upstream status — stated precisely, because the sibling fix is a
trap.** This operand-adjustment guard is still `if (decode->e.intercept)`
in the 10.0.0 tree I built *and* in current `master` — verified by
reading `decode-new.c.inc` (the `X86_TYPE_C`/`X86_TYPE_D` cases), **not**
by running a `master` build. So the accurate claim is: a *second
instance* of the exact pattern Bonzini fixed at the dispatch site is
still open at the operand site; it is not a fresh, unrelated discovery.
The runtime evidence that it is live on the shipped Debian binary is the
`--flat32` reproducer: on QEMU **10.0.11** it loads `CR3 = 0x30000` and
reads it straight back as `0x024c8000` (host/shadow CR3), no kernel
involved. Any upstream report should lead with that framing.

**Honest scope.** The failure needs the whole tower — QEMU-TCG emulating
SVM, `kvm_amd` doing shadow paging on top of it — so it is a corner
almost nothing else exercises, and none of it is embervisor's fault.
What the rig proved is narrower and solid: given a real `/dev/kvm`,
embervisor's boot path is correct, and the triple fault was the platform
underneath lying to the guest about CR3.

## 9. Scope cuts, and what each would cost

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
