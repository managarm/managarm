ENTRY(eirEntry)

SECTIONS {
	. = 0x40080000;
	eirImageFloor = .;

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
		eirBssStart = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(8);
		eirBssEnd = .;
	}

	eirImageCeiling = .;
}

