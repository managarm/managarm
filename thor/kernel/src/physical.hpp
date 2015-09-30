
namespace thor {

struct Chunk {
	enum : uint8_t {
		// Marks entries that have only white children
		kColorWhite = 0,
		// Marks entries that have only black children
		kColorBlack = 1,
		// Marks entries that have black AND white children
		kColorGray = 2,
		// Marks entries that are reserved and cannot be used
		// i.e. non-existant memory locations
		kColorRed = 3,
		
		kEntryShift = 2,
		kEntryMask = 3,
		kEntriesPerByte = 4,
		
		kMultiplicator = 1,

		// Number of entries corresponding to a single entry at a lower level
		// Must be a multiple of kEntriesPerByte
		kGranularity = kMultiplicator * kEntriesPerByte,

		// Number of bytes in the lowest level
		kBytesInRoot = 2
	};
	
	static size_t numBytesInLevel(int level);
	static size_t numEntriesInLevel(int level);
	static size_t offsetOfLevel(int level);
	
	size_t pagesPerEntry(int level);
	size_t spacePerEntry(int level);

	PhysicalAddr baseAddress;
	size_t pageSize;
	size_t numPages;

	int treeHeight;
	uint8_t *bitmapTree;

	Chunk(PhysicalAddr base_addr, size_t page_size, size_t num_pages);

	size_t calcBitmapTreeSize();
	void setupBitmapTree(uint8_t *bitmap_tree);
	
	void markColor(int level, int entry_in_level, uint8_t color);
	void checkNeighbors(int level, int entry_in_level,
			bool &all_white, bool &all_black_or_red, bool &all_red);
	void markGrayRecursive(int level, int entry_in_level);
	void markBlackRecursive(int level, int entry_in_level);
	void markWhiteRecursive(int level, int entry_in_level);
};

class PhysicalChunkAllocator {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	PhysicalChunkAllocator(PhysicalAddr bootstrap_base,
			size_t bootstrap_length);
	
	void addChunk(PhysicalAddr chunk_base, size_t chunk_length);
	void bootstrap();

	PhysicalAddr allocate(Guard &guard, size_t num_pages);
	void free(Guard &guard, PhysicalAddr address);

	size_t numUsedPages();
	size_t numFreePages();

	Lock lock;

private:
	void *bootstrapAlloc(size_t length, size_t alignment);

	PhysicalAddr p_bootstrapBase;
	size_t p_bootstrapLength;
	PhysicalAddr p_bootstrapPtr;

	Chunk *p_root;

	size_t p_usedPages;
	size_t p_freePages;
};

} // namespace thor

