
ENTRY(thorRtEntry)

SECTIONS {
	. = 0xFFFFFFFF80000000;

	.text : ALIGN(0x1000) {
		*(.text)
	}
	
	.data : ALIGN(0x1000) {
		*(.data)
		*(.bss)
	}
	
	.trampoline : ALIGN(0x1000) {
		trampolineStart = .;
		*(.trampoline)
		. = ALIGN(0x1000);
		trampolineEnd = .;
	}
}

