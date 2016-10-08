
#include <limits.h>

#include <frigg/macros.hpp>
#include <frigg/optional.hpp>
#include <frigg/variant.hpp>

namespace frigg FRIGG_VISIBILITY {

namespace _buddy {
	inline unsigned long ceilTo2Power(unsigned long x) {
		assert(x && x <= (ULONG_MAX / 2) + 1);
		return 1 << (sizeof(unsigned long) * CHAR_BIT - __builtin_clzl(x - 1));
	}
	
	// ----------------------------------------------------
	// Facet code
	// ----------------------------------------------------

	struct BitFacet {
		typedef unsigned int BitElement;

		static constexpr int kBitsInElement = sizeof(BitElement) * CHAR_BIT;

		BitFacet(BitElement *elements)
		: elements(elements) { }
	
		bool testBit(uintptr_t b) {
			uintptr_t e = b / kBitsInElement;
			BitElement m = BitElement(1) << (b % kBitsInElement);
			return elements[e] & m;
		}
		bool clearBit(uintptr_t b) {
			uintptr_t e = b / kBitsInElement;
			BitElement m = BitElement(1) << (b % kBitsInElement);
			return elements[e] &= ~m;
		}

		// bitset. a one bit in this set means that the corresponding page
		// is available for allocation.
		BitElement *elements;
	};

	struct AggregateFacet {
		AggregateFacet(int *elements)
		: elements(elements) { }
		
		int *elements;
	};

	// ----------------------------------------------------
	// Layer, Chunk and Allocator code
	// ----------------------------------------------------

	using AnyFacet = Variant<
		BitFacet,
		AggregateFacet
	>;

	struct Layer {
		Layer(int shift, size_t num_pages, AnyFacet facet)
		: shift(shift), numPages(num_pages), facet(facet) { }
		
		// each page in this layer has a size of (size_t(1) << shift).
		int shift;

		// number of pages in this layer.
		size_t numPages;

		// facet that stores which pages are allocated/free.
		AnyFacet facet;
	};

	// a chunk describes a contiguous region of memory.
	// this memory is managed by multiple layers that divide the chunk
	// into pages at multiple granularities.
	struct Chunk {
		Chunk(uintptr_t base, int num_levels, Layer *layer)
		: base(base), numLevels(num_levels), layers(layer) { }

		// base address of the memory region represented by this chunk.
		uintptr_t base;

		// number of layers in the array below.
		int numLevels;

		// array of layers that represent this chunk.
		Layer *layers;
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

				// the finest layer is stored as a bitset.
				if(s == fine_shift) {
					size_t num_elements = (num_pages + BitFacet::kBitsInElement - 1)
							/ BitFacet::kBitsInElement;
					overhead += sizeof(Layer) + sizeof(BitFacet::BitElement) * num_elements;
				}else{
					overhead += sizeof(Layer) + sizeof(int) * num_pages;
				}
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
			auto layers = (Layer *)reserve(intern, limit, sizeof(Layer) * num_levels);

			for(int i = 0; i < num_levels; i++) {
				int shift = coarse_shift - i;
				size_t num_pages = chunk_length >> shift;

				// the finest layer is stored as a bitset.
				if(shift == fine_shift) {
					size_t num_elements = (num_pages + BitFacet::kBitsInElement - 1)
							/ BitFacet::kBitsInElement;
					size_t set_size = sizeof(BitFacet::BitElement) * num_elements;
					auto elements = (BitFacet::BitElement *)reserve(intern, limit, set_size);
					memset(elements, 0xFF, set_size);

					new (&layers[i]) Layer(shift, num_pages, BitFacet(elements));
				}else{
					auto elements = (int *)reserve(intern, limit, sizeof(int) * num_pages);
					for(size_t e = 0; e < num_pages; e++)
						elements[e] = shift;
					
					new (&layers[i]) Layer(shift, num_pages, AggregateFacet(elements));
				}
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

			switch(layer->facet.tag()) {
			case AnyFacet::tagOf<BitFacet>(): {
				auto facet = &layer->facet.get<BitFacet>();
				assert(size == size_t(1) << layer->shift);
				
				size_t k = 0;
				while(k < n && !facet->testBit(f + k))
					k++;

				// note that this can only happen in the first level.
				if(k == n)
					return nullOpt;
			
				facet->clearBit(f + k);

				return AllocateSuccess(uintptr_t(f + k) << layer->shift, 0);
			}
			case AnyFacet::tagOf<AggregateFacet>(): {
				auto facet = &layer->facet.get<AggregateFacet>();
				if(size == size_t(1) << layer->shift) {
					size_t k = 0;
					while(k < n && (size_t(1) << facet->elements[f + k]) < size)
						k++;

					// note that this can only happen in the first level.
					if(k == n)
						return nullOpt;

					assert(facet->elements[f + k] == layer->shift);
					facet->elements[f + k] = 0;
					
					int bank_shift = 0;
					for(size_t i = 0; i < n; i++)
						bank_shift = max(bank_shift, facet->elements[f + i]);
					return AllocateSuccess(uintptr_t(f + k) << layer->shift, bank_shift);
				}else{
					size_t k = 0;
					while(k < n && (size_t(1) << facet->elements[f + k]) < size)
						k++;

					// note that this can only happen in the first level.
					if(k == n)
						return nullOpt;

					// note: we can be sure that this allocation succeeds;
					// otherwise the corresponding region would have been marked as allocated.
					auto result = _allocateInLayer(size, chunk, level + 1,
							uintptr_t(f + k) << layer->shift, size_t(1) << layer->shift);
					assert(result);
					
					facet->elements[f + k] = result->bankShift;
		
					int bank_shift = 0;
					for(size_t i = 0; i < n; i++)
						bank_shift = max(bank_shift, facet->elements[f + i]);
					return AllocateSuccess(result->offset, bank_shift);
				}
			}
			default:
				assert(!"AnyFacet: Unexpected tag");
			}
		}

		Chunk *_singleChunk;
	};
}

using BuddyAllocator = _buddy::Allocator;

} // namespace frigg

