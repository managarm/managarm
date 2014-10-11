
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;

typedef uint32_t addr32_t;
typedef uint16_t addr16_t;
typedef uint64_t addr64_t;

int print_x = 0;
int print_y = 0;
static const int print_width = 80;
static const int print_height = 25;

extern char pkrt_image;

enum PageFlags {
	kPagePresent = 1,
	kPageWrite = 2
};

void print_put(int x, int y, char c) {
	char *vidmem = (char*)0xB8000;
	vidmem[(y * print_width + x) * 2] = c;
	vidmem[(y * print_width + x) * 2 + 1] = 0x0F;
}
void print_copy(int dest_x, int dest_y, int src_x, int src_y) {
	char *vidmem = (char*)0xB8000;
	vidmem[(dest_y * print_width + dest_x) * 2]
			= vidmem[(src_y * print_width + src_x) * 2];
	vidmem[(dest_y * print_width + dest_x) * 2 + 1]
			= vidmem[(src_y * print_width + src_x) * 2 + 1];
}

void print_scroll() {
	for(int y = 0; y < print_height - 1; y++)
		for(int x = 0; x < print_width; x++)
			print_copy(x, y, x, y + 1);
	for(int x = 0; x < print_width; x++)
		print_put(x, print_height - 1, ' ');
}

void print_chr(char c) {
	if(c == '\n') {
		print_y++;
		print_x = 0;
		if(print_y == print_height) {
			print_scroll();
			print_y = print_height - 1;
		}
	}else{
		print_put(print_x, print_y, c);
		print_x++;
		if(print_x == print_width) {
			print_y++;
			print_x = 0;
			if(print_y == print_height) {
				print_scroll();
				print_y = print_height - 1;
			}
		}
	}
}
void print_str(char *string) {
	while(*string != 0)
		print_chr(*(string++));
}

void print_uint(unsigned int num, int radix) {
	if(num == 0) {
		print_chr('0');
		return;
	}
	char *digits = "0123456789abcdef";
	int log = 0;
	unsigned int rem = num;
	while(1) {
		rem /= radix;
		if(rem == 0)
			break;
		log++;
	}
	unsigned int p = 1;
	for(int i = 0; i < log; i++)
		p *= radix;
	while(p > 0) {
		int d = num / p;
		print_chr(digits[d]);
		num %= p;
		p /= radix;
	}
}

void pk_panic() {
	while(1) { }
}

addr32_t next_page = 0x300000;

addr32_t alloc_page() {
	addr32_t page = next_page;
	next_page += 0x1000;
	return page;
}

addr32_t pk_pml4 = 0;

void pk_page_setup() {
	pk_pml4 = alloc_page();
	for(int i = 0; i < 512; i++)
		((uint64_t*)pk_pml4)[i] = 0;
}

void pk_page_map4k(addr64_t virtual, addr64_t physical) {
	if(virtual % 0x1000 != 0) {
		print_str("pk_page_map(): Illegal virtual address alignment");
		pk_panic();
	}
	if(physical % 0x1000 != 0) {
		print_str("pk_page_map(): Illegal physical address alignment");
		pk_panic();
	}

	int pml4_index = (int)((virtual >> 39) & 0x1FF);
	int pdpt_index = (int)((virtual >> 30) & 0x1FF);
	int pd_index = (int)((virtual >> 21) & 0x1FF);
	int pt_index = (int)((virtual >> 12) & 0x1FF);
	
	/* find the pml4_entry. the pml4 is always present */
	addr32_t pml4 = pk_pml4;
	uint64_t pml4_entry = ((uint64_t*)pml4)[pml4_index];
	
	/* find the pdpt entry; create pdpt if necessary */
	addr32_t pdpt = (addr32_t)(pml4_entry & 0xFFFFF000);
	if((pml4_entry & kPagePresent) == 0) {
		pdpt = alloc_page();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pdpt)[i] = 0;
		((uint64_t*)pml4)[pml4_index] = pdpt | kPagePresent | kPageWrite;
	}
	uint64_t pdpt_entry = ((uint64_t*)pdpt)[pdpt_index];
	
	/* find the pd entry; create pd if necessary */
	addr32_t pd = (addr32_t)(pdpt_entry & 0xFFFFF000);
	if((pdpt_entry & kPagePresent) == 0) {
		pd = alloc_page();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pd)[i] = 0;
		((uint64_t*)pdpt)[pdpt_index] = pd | kPagePresent | kPageWrite;
	}
	uint64_t pd_entry = ((uint64_t*)pd)[pd_index];
	
	/* find the pt entry; create pt if necessary */
	addr32_t pt = (addr32_t)(pd_entry & 0xFFFFF000);
	if((pd_entry & kPagePresent) == 0) {
		pt = alloc_page();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pt)[i] = 0;
		((uint64_t*)pd)[pd_index] = pt | kPagePresent | kPageWrite;
	}
	uint64_t pt_entry = ((uint64_t*)pt)[pt_index];
	
	/* setup the new pt entry */
	if((pt_entry & kPagePresent) != 0) {
		print_str("pk_page_map(): Page already mapped!\n");
		print_str("   page: 0x");
		print_uint(virtual, 16);
		print_str("\n");
		pk_panic();
	}
	((uint64_t*)pt)[pt_index] = physical | kPagePresent | kPageWrite;
}

void pkrt_lgdt(addr32_t gdt_page, uint32_t size);
void pkrt_lidt(addr32_t idt_page, uint32_t size);
void pkrt_kernel(uint32_t pml4, addr64_t entry);

void pk_init_gdt() {
	addr32_t gdt_page = alloc_page();
	uint8_t *gdt = (uint8_t*)gdt_page;
	gdt[0] = 0;
	gdt[1] = 0;
	gdt[2] = 0;
	gdt[3] = 0;
	gdt[4] = 0;
	gdt[5] = 0;
	gdt[6] = 0;
	gdt[7] = 0;
	
	/* 32-bit code */
	gdt[8] = 0xFF; /* segment limit 0-7 */
	gdt[9] = 0xFF; /* segment limit 8-15 */
	gdt[10] = 0; /* segment base 0-7 */
	gdt[11] = 0; /* segment base 8-15 */
	gdt[12] = 0; /* segment base 16-23 */
	gdt[13] = 0x98; /* flags */
	gdt[14] = 0xCF; /* segment limit 16-19 + flags */
	gdt[15] = 0; /* segment base 24-31 */
	
	/* 32-bit data */
	gdt[16] = 0xFF; /* segment limit 0-7 */
	gdt[17] = 0xFF; /* segment limit 8-15 */
	gdt[18] = 0; /* segment base 0-7 */
	gdt[19] = 0; /* segment base 8-15 */
	gdt[20] = 0; /* segment base 16-23 */
	gdt[21] = 0x92; /* flags */
	gdt[22] = 0xCF; /* segment limit 16-19 + flags */
	gdt[23] = 0; /* segment base 24-31 */
	
	/* 64-bit code */
	gdt[24] = 0xFF; /* segment limit 0-7 */
	gdt[25] = 0xFF; /* segment limit 8-15 */
	gdt[26] = 0; /* segment base 0-7 */
	gdt[27] = 0; /* segment base 8-15 */
	gdt[28] = 0; /* segment base 16-23 */
	gdt[29] = 0x98; /* flags */
	gdt[30] = 0xAF; /* segment limit 16-19 + flags */
	gdt[31] = 0; /* segment base 24-31 */

	pkrt_lgdt(gdt_page, 31); 
}
void pk_init_idt() {

}

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

const Elf64_Half ET_NONE = 0;
const Elf64_Half ET_EXEC = 2;

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

void pk_load_image(addr64_t *out_entry) {
	addr32_t image = (addr32_t)(&pkrt_image);
	
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image;
	if(ehdr->e_ident[0] != '\x7F'
			|| ehdr->e_ident[1] != 'E'
			|| ehdr->e_ident[2] != 'L'
			|| ehdr->e_ident[3] != 'F') {
		print_str("Illegal magic fields");
		pk_panic();
	}
	if(ehdr->e_type != ET_EXEC) {
		print_str("Kernel image must be ET_EXEC");
		pk_panic();
	}
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		Elf64_Phdr *phdr = (Elf64_Phdr*)(image + (addr32_t)ehdr->e_phoff
				+ i * ehdr->e_phentsize);
		if(phdr->p_offset % 0x1000 != 0) {
			print_str("PHDR not aligned in file");
			pk_panic();
		}else if(phdr->p_vaddr % 0x1000 != 0) {
			print_str("PHDR not aligned in memory");
			pk_panic();
		}else if(phdr->p_filesz != phdr->p_memsz) {
			print_str("PHDR file size != memory size");
			pk_panic();
		}
		
		uint32_t page = 0;
		while(page < (uint32_t)phdr->p_filesz) {
			pk_page_map4k(phdr->p_vaddr + page,
					image + (uint32_t)phdr->p_offset + page);
			page += 0x1000;
		}
		print_str("loaded phdr\n");
	}
	
	*out_entry = ehdr->e_entry;
}

void prekernel_main() {
	print_str("pk: initializing\n");
	print_uint(0xf0000001, 16);
	print_str("\n");

	if(sizeof(void*) != 4
			|| sizeof(uint32_t) != 4
			|| sizeof(uint64_t) != 8)
		print_str("Invalid environment");

	pk_init_gdt();
	pk_init_idt();

	pk_page_setup();
	for(addr32_t addr = 0; addr < 0x800000; addr += 0x1000)
		pk_page_map4k(addr, addr);

	addr64_t kernel_entry;
	pk_load_image(&kernel_entry);

	pkrt_kernel(pk_pml4, kernel_entry);
	
	print_str(" init paging");
}

