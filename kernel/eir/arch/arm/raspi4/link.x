ENTRY(eirRtEntry)

SECTIONS {
	. = 0x80000;

	.text : ALIGN(0x1000) {
		*(.text.init)
		*(.text*)
	}

	.rodata : ALIGN(0x1000) {
		*(.rodata*)
	}

	.data : ALIGN(0x1000) {
		*(.data*)
	}

	.bss : ALIGN(0x1000) {
		eirRtBssStart = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(8);
		eirRtBssEnd = .;
	}

	eirRtImageCeiling = .;
}

