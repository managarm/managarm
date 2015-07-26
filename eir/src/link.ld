
TARGET(elf32-i386)
OUTPUT_FORMAT(elf32-i386)
ENTRY(eirRtEntry)

SECTIONS {
	. = 0x100000;

	.text : ALIGN(4) {
		*(.header)
		*(.text)
	}
	.data : ALIGN(0x1000) {
		*(.data)
		*(.image)
	}
	.bss : ALIGN(0x1000) {
		*(.bss)
	}

	eirRtImageCeiling = .;
}

