
namespace thor {

struct SkeletalRegion {
public:
	static void initialize(PhysicalAddr physical_base,
			int order, size_t num_roots, int8_t *buddy_tree);

	static SkeletalRegion &global();

	// TODO: make this private
	SkeletalRegion() = default;

	SkeletalRegion(const SkeletalRegion &other) = delete;
	
	SkeletalRegion &operator= (const SkeletalRegion &other) = delete;

	PhysicalAddr allocate();
	void deallocate(PhysicalAddr phyiscal);

	void *access(PhysicalAddr physical);

private:
	PhysicalAddr _physicalBase;
	int _order;
	size_t _numRoots;
	int8_t *_buddyTree;
};

class PhysicalChunkAllocator {
	typedef frigg::TicketLock Mutex;
public:
	PhysicalChunkAllocator();
	
	void bootstrap(PhysicalAddr address,
			int order, size_t num_roots, int8_t *buddy_tree);

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

