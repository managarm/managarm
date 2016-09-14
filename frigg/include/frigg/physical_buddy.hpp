
#include <limits.h>

#include <frigg/optional.hpp>

namespace frigg {

namespace _buddy {
//	typedef unsigned int BitElement;

//	constexpr int kBitsInElement = sizeof(BitElement) * CHAR_BIT;

/*	struct BitLayer {
		BitLayer(int shift, size_t num_bits)
		: shift(shift), numBits(num_bits),
				fullSet(nullptr), partialSet(nullptr) { }

		// each bit in the sets below represents a memory region of size (size_t(1) << shift).
		int shift;

		// number of bits in the sets below.
		size_t numBits;

		// bitset. a one bit in this set means that the whole corresponding region
		// is available for allocation.
		BitElement *fullSet;

		// bitset. a one bit in this set means that parts of the corresponding region
		// are available for allocation.
		BitElement *partialSet;
	};*/

	inline unsigned long ceilTo2Power(unsigned long x) {
		assert(x && x <= (ULONG_MAX / 2) + 1);
		return 1 << (sizeof(unsigned long) * CHAR_BIT - __builtin_clzl(x - 1));
	}

	struct CoverLayer {
		CoverLayer(int shift, size_t num_pages, int *elements)
		: shift(shift), numPages(num_pages), elements(elements) { }
		
		int shift;

		size_t numPages;

		int *elements;
	};

/*	inline bool testBit(BitElement *set, uintptr_t b) {
		uintptr_t e = b / kBitsInElement;
		BitElement m = BitElement(1) << (b % kBitsInElement);
		return set[e] & m;
	}
	inline bool testSomeBits(BitElement *set, uintptr_t f, size_t n) {
		for(size_t i = 0; i < n; i++)
			if(testBit(set, f + i))
				return true;
		return false;
	}

	inline bool clearBit(BitElement *set, uintptr_t b) {
		uintptr_t e = b / kBitsInElement;
		BitElement m = BitElement(1) << (b % kBitsInElement);
		return set[e] &= ~m;
	}*/

	struct Chunk {
		Chunk(uintptr_t base, int num_levels, CoverLayer *layer)
		: base(base), numLevels(num_levels), layers(layer) { }

		// base address of the memory region represented by this chunk.
		uintptr_t base;

		// number of layers in the array below.
		int numLevels;

		// array of layers that represent this chunk.
		CoverLayer *layers;
	};

	inline void *reserve(void *&intern, void *limit, size_t size) {
		void *p = intern;
		intern = (char *)intern + size;
		assert(intern <= limit);
		return p;
	}

	struct AllocateSuccess {
		AllocateSuccess(uintptr_t offset, int bank_shift)
		: offset(offset), bankShift(bank_shift) { }

		uintptr_t offset;
		int bankShift;
	};

	struct Allocator {
		static size_t computeOverhead(size_t chunk_length,
				int fine_shift, int coarse_shift) {
			size_t overhead = sizeof(Chunk);
			for(int s = fine_shift; s <= coarse_shift; s++) {
				size_t num_pages = chunk_length >> s;
//------------------------------------------------
//				size_t num_elements = ((chunk_length >> s)
//							+ kBitsInElement - 1) / kBitsInElement;
//				overhead += sizeof(BitLayer) + 2 * sizeof(BitElement) * num_elements;
//------------------------------------------------
				overhead += sizeof(CoverLayer) + sizeof(int) * num_pages;
			}
			return overhead;
		}

		Allocator()
		: _singleChunk(nullptr) { }

		void addChunk(uintptr_t chunk_base, size_t chunk_length,
				int fine_shift, int coarse_shift, void *intern) {
			assert(!(chunk_base % (uintptr_t(1) << coarse_shift)));
			assert(!(chunk_length % (uintptr_t(1) << coarse_shift)));

			void *limit = (char *)intern + computeOverhead(chunk_length,
					fine_shift, coarse_shift);

			int num_levels = coarse_shift - fine_shift + 1;
//------------------------------------------
/*			auto layers = (BitLayer *)reserve(intern, limit, sizeof(BitLayer) * num_levels);

			for(int i = 0; i < num_levels; i++) {
				size_t shift = fine_shift + i;
				size_t num_bits = chunk_length >> shift;
				auto layer = new (&layers[i]) BitLayer(shift, num_bits);
				
				size_t num_elements = (num_bits + kBitsInElement - 1) / kBitsInElement;
				size_t set_size = sizeof(BitElement) * num_elements;
				layer->fullSet = (BitElement *)reserve(intern, limit, set_size);
				layer->partialSet = (BitElement *)reserve(intern, limit, set_size);
				memset(layer->fullSet, 0xFF, set_size);
				memset(layer->partialSet, 0xFF, set_size);
			}
*/
//------------------------------------------
			auto layers = (CoverLayer *)reserve(intern, limit, sizeof(CoverLayer) * num_levels);
			for(int i = 0; i < num_levels; i++) {
				size_t shift = coarse_shift - i;
				size_t num_pages = chunk_length >> shift;

				auto elements = (int *)reserve(intern, limit, sizeof(int) * num_pages);
				for(size_t e = 0; e < num_pages; e++)
					elements[e] = shift;
				
				new (&layers[i]) CoverLayer(shift, num_pages, elements);
			}
			
			assert(!_singleChunk);
			_singleChunk = new (reserve(intern, limit, sizeof(Chunk)))
					Chunk(chunk_base, num_levels, layers);
		}

		uintptr_t allocate(size_t size) {
			return _allocateInChunk(ceilTo2Power(size), _singleChunk);
		}

	private:
		uintptr_t _allocateInChunk(size_t size, Chunk *chunk) {
			auto result = _allocateInLayer(size, chunk, 0,
					0, chunk->layers[0].numPages << chunk->layers[0].shift);
			assert(result);
			return chunk->base + result->offset;
		}

		Optional<AllocateSuccess> _allocateInLayer(size_t size, Chunk *chunk, int level,
				uintptr_t bank_offset, size_t bank_size) {
			assert(level < chunk->numLevels);
			auto layer = &chunk->layers[level];

			assert(size <= size_t(1) << layer->shift);

			assert(!(bank_offset % (uintptr_t(1) << layer->shift)));
			assert(!(bank_size % (size_t(1) << layer->shift)));
			uintptr_t f = bank_offset >> layer->shift;
			size_t n = bank_size >> layer->shift;

//------------------------------------------
/*			BitLayer *layer = &chunk->layers[level];

			if(size == size_t(1) << layer->shift) {
				// we allocate a memory region from this layer.
				// first we allocate a bit from the given bank.
				size_t k = 0;
				while(k < n && !testBit(layer->fullSet, f + k))
					k++;

				// note that this can only happen in the first level.
				assert(k < n);
//				if(k == n)
//					return nullOpt;
			
				clearBit(layer->fullSet, f + k);
				clearBit(layer->partialSet, f + k);

				return AllocateSuccess(!testSomeBits(layer->partialSet, f, n),
						uintptr_t(f + k) << layer->shift);
			}else{
				size_t k = 0;
				while(k < n && !testBit(layer->partialSet, f + k))
					k++;

				// note that this can only happen in the first level.
				if(k == n)
					return nullOpt;

				// note: we can be sure that this allocation succeeds;
				// otherwise the corresponding region would have been marked as allocated.
				auto result = _allocateInLayer(size, chunk, level + 1,
						uintptr_t(f + k) << layer->shift, size_t(1) << layer->shift);
				assert(result);
				
				clearBit(layer->fullSet, f + k);
				if(result->bankFull)
					clearBit(layer->partialSet, f + k);

				return AllocateSuccess(!testSomeBits(layer->partialSet, f, n),
						result->offset);
			}*/
//------------------------------------------
			if(size == size_t(1) << layer->shift) {
				size_t k = 0;
				while(k < n && (size_t(1) << layer->elements[f + k]) < size)
					k++;

				// note that this can only happen in the first level.
				if(k == n)
					return nullOpt;

				assert(layer->elements[f + k] == layer->shift);
				layer->elements[f + k] = 0;
				
				int bank_shift = 0;
				for(size_t i = 0; i < n; i++)
					bank_shift = max(bank_shift, layer->elements[f + i]);
				return AllocateSuccess(uintptr_t(f + k) << layer->shift, bank_shift);
			}else{
				size_t k = 0;
				while(k < n && (size_t(1) << layer->elements[f + k]) < size)
					k++;

				// note that this can only happen in the first level.
				if(k == n)
					return nullOpt;

				// note: we can be sure that this allocation succeeds;
				// otherwise the corresponding region would have been marked as allocated.
				auto result = _allocateInLayer(size, chunk, level + 1,
						uintptr_t(f + k) << layer->shift, size_t(1) << layer->shift);
				assert(result);
				
				layer->elements[f + k] = result->bankShift;
	
				int bank_shift = 0;
				for(size_t i = 0; i < n; i++)
					bank_shift = max(bank_shift, layer->elements[f + i]);
				return AllocateSuccess(result->offset, bank_shift);
			}
		}

		Chunk *_singleChunk;
	};
}

using BuddyAllocator = _buddy::Allocator;

} // namespace frigg

