ENTRY(efiStart)
/* OUTPUT_FORMAT(elf64-riscv64) */

PHDRS {
	text	PT_LOAD	FLAGS((1 << 0) | (1 << 2)); /* RX */
	rodata	PT_LOAD	FLAGS((1 << 2)); /* R */
	data	PT_LOAD	FLAGS((1 << 1) | (1 << 2)); /* RW */
}

SECTIONS {
	/* with EFI, eir starts in the higher half. */
	. = 0xffffffff80000000;
	eirImageFloor = .;

	.text : ALIGN(0x1000) {
		*(.text*)
	} : text
	.rodata : ALIGN(0x1000) {
		*(.rodata*)
	} : rodata
	.data : ALIGN(0x1000) {
		*(.data*)
	} : data
	.init_array : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP (*(.init_array .ctors))
		PROVIDE_HIDDEN (__init_array_end = .);
	}
	.bss : ALIGN(0x1000) {
		*(.bss*)
		*(COMMON)
	} : data

	eirImageCeiling = .;

	/DISCARD/ : {
		*(.eh_frame)
		*(.note*)
	}
}

