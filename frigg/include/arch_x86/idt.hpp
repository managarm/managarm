
namespace frigg {
namespace arch_x86 {

enum IdtFlags : uint32_t {
	kIdtWord1InterruptGate = 0x0E00,
	kIdtWord1Present = 0x8000
};

struct Idtr {
	uint16_t limit;
	uint64_t pointer;
} __attribute__ (( packed ));

void makeIdt64NullGate(uint32_t *idt, int entry);
void makeIdt64IntGate(uint32_t *idt, int entry, int segment, void *handler);

}} // namespace frigg::arch_x86

