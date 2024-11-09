
ENTRY(thorRtEntry)

SECTIONS {
	/* thor starts a bit higher than normal, to make space for eir on EFI systems. */
	. = 0xffffffff90000000;

	.text ALIGN(0x1000) : { *(.text .text.*) }
	.rodata ALIGN(0x1000) : { *(.rodata .rodata.*) }
	.eh_frame_hdr ALIGN(0x1000) : { *(.eh_frame_hdr) }
	.eh_frame : { *(.eh_frame) }

	.init_array ALIGN(0x1000) : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP (*(.init_array .ctors))
		PROVIDE_HIDDEN (__init_array_end = .);
	}

	.data ALIGN(0x1000) : { *(.data) }
	.bss : { *(.bss) }
}

