
ENTRY(thorRtEntry)

SECTIONS {
	/* this address is dictated by system v abi's kernel code model */
	. = 0xFFFFFFFF80000000;

	.text : ALIGN(0x1000) {
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.eh_frame)
		*(.ctors)
	}
	
	.data : ALIGN(0x1000) {
		*(.data)
		*(.bss)
		. = ALIGN(0x1000);
	}
	
	/* note: we have some freedom where to place the trampoline area in virtual memory. 
	 * it must be located in the first 16 KiB of memory (so that we can just zero
	 * the real mode segment registers and work with linear addresses directly)
	 * and it must be aligned to a page (as required by the startup ipi) */
	. = 0x1000;

	.trampoline : AT(LOADADDR(.data) + SIZEOF(.data)) ALIGN(0x1000) {
		*(.trampoline)
		. = ALIGN(0x1000);
	}

	_trampoline_startLma = LOADADDR(.trampoline);
	_trampoline_endLma = LOADADDR(.trampoline) + SIZEOF(.trampoline);
}

