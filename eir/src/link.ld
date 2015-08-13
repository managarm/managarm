
TARGET(elf32-i386)
OUTPUT_FORMAT(elf32-i386)
ENTRY(eirRtEntry)

SECTIONS {
	. = 0x100000;

	.text : ALIGN(0x1000) {
		*(.header)
		*(.text)
	}
	
	.trampoline : ALIGN(0x100) {
		trampolineStart = .;
		*(.trampoline)
		. = ALIGN(0x1000);
		trampolineEnd = .;
	}

	.data : ALIGN(0x1000) {
		*(.data)
	}
	.bss : ALIGN(0x1000) {
		*(.bss)
	}

	eirRtImageCeiling = .;
}

