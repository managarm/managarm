
namespace thor {
namespace memory {

class PhysicalAllocator {
public:
	virtual uintptr_t allocate(size_t num_pages) = 0;
};

class StupidPhysicalAllocator : public PhysicalAllocator {
public:
	StupidPhysicalAllocator(uintptr_t next_page);
	
	virtual uintptr_t allocate(size_t num_pages);

private:
	uintptr_t p_nextPage;
};

extern PhysicalAllocator *tableAllocator;

}} // namespace thor::memory

