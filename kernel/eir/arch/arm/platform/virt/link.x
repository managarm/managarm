ENTRY(eirEntry)

SECTIONS {
	. = 0;
	eirImageFloor = .;

	.text : ALIGN(0x1000) {
		*(.text.init)
		*(.text*)
	}

	.rodata : ALIGN(0x1000) {
		*(.rodata*)
	}
	.init_array : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP (*(.init_array .ctors))
		PROVIDE_HIDDEN (__init_array_end = .);
	}

	.dynamic : {
		*(.dynamic)
	}

	.data : ALIGN(0x1000) {
		*(.data*)
	}

	.bss : ALIGN(0x1000) {
		eirBssStart = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(8);
		eirBssEnd = .;
	}

	eirImageCeiling = .;
}

