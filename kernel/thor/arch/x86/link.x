
ENTRY(thorRtEntry)

SECTIONS {
	/* This address is dictated by System V ABI's kernel code model. */
	. = 0xFFFFFFFF80000000;

	stubsPtr = .;
	.text.stubs : {
		*(.text.stubs)
	}
	stubsLimit = .;

	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame : { *(.eh_frame) }

	. = ALIGN(0x1000);

	.init_array : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP (*(.init_array .ctors))
		PROVIDE_HIDDEN (__init_array_end = .);
	}
	.data : { *(.data) }
	.bss : { *(.bss) }
}

