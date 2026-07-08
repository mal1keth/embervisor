# SPDX-License-Identifier: MIT
#
# The smallest possible guest: 16-bit real-mode code, loaded flat at
# 0x1000, that writes a string to the COM1 data port and halts.
#
# There is no BIOS, no firmware, no OS underneath this. The first
# instruction KVM executes after VMENTER is `cli` below. Every `out`
# causes a VM exit that embervisor's run loop services; `hlt` (with no
# in-kernel irqchip) exits to userspace and ends the VM.

	.code16
	.section .text
	.globl _start
_start:
	cli
	mov	$msg, %si		# DS:SI -> string (DS base is 0)
	mov	$0x3f8, %dx		# COM1 THR
1:
	lodsb				# AL = *SI++
	test	%al, %al
	jz	2f
	out	%al, (%dx)		# VM exit: KVM_EXIT_IO, port 0x3f8
	jmp	1b
2:
	hlt				# VM exit: KVM_EXIT_HLT, we're done
	jmp	2b

msg:
	.asciz	"Hello from a hardware-virtualized guest!\r\nThis text left the VM through an emulated 8250 UART, one KVM_EXIT_IO at a time.\r\n"
