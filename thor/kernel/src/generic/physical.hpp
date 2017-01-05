
namespace thor {

class PhysicalChunkAllocator {
	typedef frigg::TicketLock Mutex;
public:
	PhysicalChunkAllocator();
	
	void bootstrap(PhysicalAddr address, int order, size_t num_roots);

	PhysicalAddr allocate(size_t size);
	void free(PhysicalAddr address, size_t size);

	size_t numUsedPages();
	size_t numFreePages();

private:
	Mutex _mutex;

	PhysicalAddr _physicalBase;
	int8_t *_buddyPointer;
	int _buddyOrder;
	size_t _buddyRoots;

	size_t _usedPages;
	size_t _freePages;
};

} // namespace thor

