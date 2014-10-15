
ENTRY(thorRtEntry)

SECTIONS {
	. = 0xFFFF800000000000;

	.text : ALIGN(0x1000) {
		*(.text)
	}
	.data : ALIGN(0x1000) {
		*(.data)
		*(.bss)
	}
}

