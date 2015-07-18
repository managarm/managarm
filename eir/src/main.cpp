
#include "../../frigg/include/arch_x86/types32.hpp"
#include "../../frigg/include/elf.hpp"
#include "../../frigg/include/arch_x86/gdt.hpp"

typedef uint32_t addr32_t;
typedef uint64_t addr64_t;

int print_x = 0;
int print_y = 0;
static const int print_width = 80;
static const int print_height = 25;

extern char pkrt_image;

enum PageFlags {
	kPagePresent = 1,
	kPageWrite = 2,
	kPageUser = 4
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
void print_str(const char *string) {
	while(*string != 0)
		print_chr(*(string++));
}

void print_uint(unsigned int num, int radix) {
	if(num == 0) {
		print_chr('0');
		return;
	}
	const char *digits = "0123456789abcdef";
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

addr32_t next_page = 0x4000000;

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
	
	for(int i = 256; i < 512; i++) {
		addr32_t pdpt_page = alloc_page();
		uint64_t *pdpt_pointer = (uint64_t *)pdpt_page;
		for(int j = 0; j < 512; j++)
			pdpt_pointer[j] = 0;

		((uint64_t*)pk_pml4)[i] = pdpt_page | kPagePresent | kPageWrite | kPageUser;
	}
}

void pk_page_map4k(addr64_t address, addr64_t physical) {
	if(address % 0x1000 != 0) {
		print_str("pk_page_map(): Illegal virtual address alignment");
		pk_panic();
	}
	if(physical % 0x1000 != 0) {
		print_str("pk_page_map(): Illegal physical address alignment");
		pk_panic();
	}

	int pml4_index = (int)((address >> 39) & 0x1FF);
	int pdpt_index = (int)((address >> 30) & 0x1FF);
	int pd_index = (int)((address >> 21) & 0x1FF);
	int pt_index = (int)((address >> 12) & 0x1FF);
	
	/* find the pml4_entry. the pml4 is always present */
	addr32_t pml4 = pk_pml4;
	uint64_t pml4_entry = ((uint64_t*)pml4)[pml4_index];
	
	/* find the pdpt entry; create pdpt if necessary */
	addr32_t pdpt = (addr32_t)(pml4_entry & 0xFFFFF000);
	if((pml4_entry & kPagePresent) == 0) {
		pdpt = alloc_page();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pdpt)[i] = 0;
		((uint64_t*)pml4)[pml4_index] = pdpt | kPagePresent | kPageWrite | kPageUser;
	}
	uint64_t pdpt_entry = ((uint64_t*)pdpt)[pdpt_index];
	
	/* find the pd entry; create pd if necessary */
	addr32_t pd = (addr32_t)(pdpt_entry & 0xFFFFF000);
	if((pdpt_entry & kPagePresent) == 0) {
		pd = alloc_page();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pd)[i] = 0;
		((uint64_t*)pdpt)[pdpt_index] = pd | kPagePresent | kPageWrite | kPageUser;
	}
	uint64_t pd_entry = ((uint64_t*)pd)[pd_index];
	
	/* find the pt entry; create pt if necessary */
	addr32_t pt = (addr32_t)(pd_entry & 0xFFFFF000);
	if((pd_entry & kPagePresent) == 0) {
		pt = alloc_page();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pt)[i] = 0;
		((uint64_t*)pd)[pd_index] = pt | kPagePresent | kPageWrite | kPageUser;
	}
	uint64_t pt_entry = ((uint64_t*)pt)[pt_index];
	
	/* setup the new pt entry */
	if((pt_entry & kPagePresent) != 0) {
		print_str("pk_page_map(): Page already mapped!\n");
		print_str("   page: 0x");
		print_uint(address, 16);
		print_str("\n");
		pk_panic();
	}
	((uint64_t*)pt)[pt_index] = physical | kPagePresent | kPageWrite | kPageUser;
}

extern "C" void pkrt_lgdt(addr32_t gdt_page, uint32_t size);
extern "C" void pkrt_lidt(addr32_t idt_page, uint32_t size);
extern "C" void pkrt_kernel(uint32_t pml4, addr64_t entry);

void pk_init_gdt() {
	addr32_t gdt_page = alloc_page();
	frigg::arch_x86::makeGdtNullSegment((uint32_t *)gdt_page, 0);
	frigg::arch_x86::makeGdtFlatCode32SystemSegment((uint32_t *)gdt_page, 1);
	frigg::arch_x86::makeGdtFlatData32SystemSegment((uint32_t *)gdt_page, 2);
	frigg::arch_x86::makeGdtCode64SystemSegment((uint32_t *)gdt_page, 3);

	pkrt_lgdt(gdt_page, 31); 
}
void pk_init_idt() {

}

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

extern "C" void prekernel_main() {
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
	for(addr32_t addr = 0; addr < 0x8000000; addr += 0x1000)
		pk_page_map4k(addr, addr);

	for(addr32_t addr = 0; addr < 1024 * 1024 * 1024; addr += 0x1000)
		pk_page_map4k(0xFFFF800100000000 + addr, addr);

	addr64_t kernel_entry;
	pk_load_image(&kernel_entry);

	pkrt_kernel(pk_pml4, kernel_entry);
	
	print_str(" init paging");
}

