#pragma once

#include <stdint.h>

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;

typedef struct {
	unsigned char e_ident[16]; /* ELF identification */
	Elf64_Half e_type; /* Object file type */
	Elf64_Half e_machine; /* Machine type */
	Elf64_Word e_version; /* Object file version */
	Elf64_Addr e_entry; /* Entry point address */
	Elf64_Off e_phoff; /* Program header offset */
	Elf64_Off e_shoff; /* Section header offset */
	Elf64_Word e_flags; /* Processor-specific flags */
	Elf64_Half e_ehsize; /* ELF header size */
	Elf64_Half e_phentsize; /* Size of program header entry */
	Elf64_Half e_phnum; /* Number of program header entries */
	Elf64_Half e_shentsize; /* Size of section header entry */
	Elf64_Half e_shnum; /* Number of section header entries */
	Elf64_Half e_shstrndx; /* Section name string table index */
} Elf64_Ehdr;

enum {
	ET_NONE = 0,
	ET_EXEC = 2,
	ET_DYN = 3
};

enum {
	SHN_UNDEF = 0,
	SHN_ABS = 0xFFF1
};

struct Elf64_Sym {
	Elf64_Word st_name;
	unsigned char st_info;
	unsigned char st_other;
	Elf64_Half st_shndx;
	Elf64_Addr st_value;
	Elf64_Xword st_size;
};

extern inline unsigned char ELF64_ST_BIND(unsigned char info) {
	return info >> 4;
}
extern inline unsigned char ELF64_ST_TYPE(unsigned char info) {
	return info & 0x0F;
}

enum {
	STB_GLOBAL = 1,
	STB_WEAK = 2
};

enum {
	STT_OBJECT = 1,
	STT_FUNC = 2,
	STT_TLS = 6
};

enum {
	R_X86_64_64 = 1,
	R_X86_64_COPY = 5,
	R_X86_64_GLOB_DAT = 6,
	R_X86_64_JUMP_SLOT = 7,
	R_X86_64_RELATIVE = 8,
	R_X86_64_DTPMOD64 = 16,
	R_X86_64_DTPOFF64 = 17,
	R_X86_64_TPOFF64 = 18,
};

enum {
	R_AARCH64_RELATIVE = 1027
};

struct Elf64_Rela {
	Elf64_Addr r_offset;
	Elf64_Xword r_info;
	Elf64_Sxword r_addend;
};

extern inline Elf64_Xword ELF64_R_SYM(Elf64_Xword info) {
	return info >> 32;
}
extern inline Elf64_Xword ELF64_R_TYPE(Elf64_Xword info) {
	return info & 0xFFFFFFFF;
}

enum {
	PT_LOAD = 1,
	PT_DYNAMIC = 2,
	PT_INTERP = 3,
	PT_NOTE = 4,
	PT_PHDR = 6,
	PT_TLS = 7,
	PT_GNU_EH_FRAME = 0x6474E550,
	PT_GNU_STACK = 0x6474E551,
	PT_GNU_RELRO = 0x6474E552
};

enum {
	PF_X = 1,
	PF_W = 2,
	PF_R = 4
};

typedef struct {
	Elf64_Word p_type; /* Type of segment */
	Elf64_Word p_flags; /* Segment attributes */
	Elf64_Off p_offset; /* Offset in file */
	Elf64_Addr p_vaddr; /* Virtual address in memory */
	Elf64_Addr p_paddr; /* Reserved */
	Elf64_Xword p_filesz; /* Size of segment in file */
	Elf64_Xword p_memsz; /* Size of segment in memory */
	Elf64_Xword p_align; /* Alignment of segment */
} Elf64_Phdr;

enum {
	DT_NULL = 0,
	DT_NEEDED = 1,
	DT_PLTRELSZ = 2,
	DT_PLTGOT = 3,
	DT_HASH = 4,
	DT_STRTAB = 5,
	DT_SYMTAB = 6,
	DT_RELA = 7,
	DT_RELASZ = 8,
	DT_RELAENT = 9,
	DT_STRSZ = 10,
	DT_SYMENT = 11,
	DT_INIT = 12,
	DT_FINI = 13,
	DT_SONAME = 14,
	DT_RPATH = 15,
	DT_SYMBOLIC = 16,
	DT_REL = 17,
	DT_BIND_NOW = 24,
	DT_INIT_ARRAY = 25,
	DT_FINI_ARRAY = 26,
	DT_INIT_ARRAYSZ = 27,
	DT_FINI_ARRAYSZ = 28,
	DT_PLTREL = 20,
	DT_DEBUG = 21,
	DT_JMPREL = 23,
	DT_FLAGS = 30,
	DT_GNU_HASH = 0x6ffffef5,
	DT_VERSYM = 0x6ffffff0,
	DT_RELACOUNT = 0x6ffffff9,
	DT_FLAGS_1 = 0x6ffffffb,
	DT_VERDEF = 0x6ffffffc,
	DT_VERDEFNUM = 0x6ffffffd,
	DT_VERNEED = 0x6ffffffe,
	DT_VERNEEDNUM = 0x6fffffff
};

enum {
	// For DT_FLAGS.
	DF_SYMBOLIC = 0x02,
	DF_STATIC_TLS = 0x10,

	// For DT_FLAGS_1.
	DF_1_NOW = 0x00000001
};

struct Elf64_Dyn {
	Elf64_Sxword d_tag;
	union {
		Elf64_Xword d_val;
		Elf64_Addr d_ptr;
	};
};
