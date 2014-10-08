
ENTRY(krt_entry)

SECTIONS {
	. = -0x100000;

	.text : ALIGN(0x1000) {
		*(.text)
	}
	.data : ALIGN(0x1000) {
		*(.data)
	}
	.bss : ALIGN(0x1000) {
		*(.bss)
	}
}

