
ENTRY(thorRtEntry)

SECTIONS {
	/* This address is dictated by System V ABI's kernel code model. */
	. = 0xFFFFFFFF80000000;

	stubsPtr = .;
	.text.stubs : {
		*(.text.stubs)
	}
	stubsLimit = .;
	.text : {
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.eh_frame)
		*(.ctors)
	}
	
	. = ALIGN(0x1000);
	.data : { *(.data) }
	.bss : { *(.bss) }
}

