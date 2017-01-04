
namespace thor {

class PhysicalChunkAllocator {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	PhysicalChunkAllocator();
	
	void bootstrap(PhysicalAddr address, int order, size_t num_roots);

	PhysicalAddr allocate(Guard &guard, size_t size);
	void free(Guard &guard, PhysicalAddr address, size_t size);

	size_t numUsedPages();
	size_t numFreePages();

	Lock lock;

private:
	PhysicalAddr _physicalBase;
	int8_t *_buddyPointer;
	int _buddyOrder;
	size_t _buddyRoots;

	size_t _usedPages;
	size_t _freePages;
};

} // namespace thor

