
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <experimental/optional>

namespace blockfs {
namespace util {

template<typename Ident, typename Entry>
struct Cache {
	struct Element {
		template<typename... Args>
		explicit Element(Args &&... args)
		: entry{std::forward<Args>(args)...},
			lockCount{0}, accessTime{0}, reuseIndex{size_t(-1)} { }

	
	//FIXME private:
		Entry entry;

		// identifier this element represents currently
		std::experimental::optional<Ident> identifier;

		// number of references to this element
		size_t lockCount;
		
		// last time this element was accessed
		// only meaningful while lockCount == 0
		uint64_t accessTime;
		
		// index of this element inside reuseQueue
		size_t reuseIndex;
	};
	
	struct Ref {
		// default constructor, move constructor, assignment operator,
		// reset() and swap() to provide move-only semantics
		Ref();
		Ref(Ref &&other);
		Ref &operator= (Ref other);
		~Ref();

		void reset();
		static void swap(Ref &a, Ref &b);
		
		Entry &operator* ();
		Entry *operator-> ();

	//FIXME private:
		explicit Ref(Cache *cache, Element *element);

		Cache *cache;
		Element *element;
	};
	
	Cache();

	void preallocate(size_t count);

	Ref lock(Ident identifier);

protected:
	virtual Element *allocate() = 0;

	virtual void initEntry(Ident identifier, Entry *entry) = 0;
	virtual void finishEntry(Entry *entry) = 0;

private:
	// implements a min-heap sorted by Element::accessTime
	std::vector<Element *> reuseQueue;

	std::unordered_map<Ident, Element *> cacheMap;

	uint64_t currentTime;
};

// --------------------------------------------------------
// Cache
// --------------------------------------------------------

template<typename Ident, typename Entry>
Cache<Ident, Entry>::Cache()
: currentTime(1) { }

template<typename Ident, typename Entry>
void Cache<Ident, Entry>::preallocate(size_t count) {
	// TODO: this invalidates the heap property if called during normal operation
	for(size_t i = 0; i < count; i++) {
		Element *element = allocate();
		element->accessTime = 0;
		element->reuseIndex = reuseQueue.size();
		reuseQueue.push_back(element);
	}
}

template<typename Ident, typename Entry>
auto Cache<Ident, Entry>::lock(Ident identifier) -> Ref {
	// try to find a matching element in cacheMap
	auto it = cacheMap.find(identifier);
	if(it != cacheMap.end()) {
		Element *element = it->second;
		assert(element->identifier && *element->identifier == identifier);
		return Ref(this, element);
	}
	
	// we have to reuse the least-recently-used element
	while(true) {
		// get the element with minimal accessTime
		assert(!reuseQueue.empty());
		Element *element = reuseQueue.front();
		element->reuseIndex = size_t(-1);

		using std::swap;
		swap(reuseQueue.front(), reuseQueue.back());
		reuseQueue.pop_back();

		// repair the heap property
		if(!reuseQueue.empty()) {
			reuseQueue.front()->reuseIndex = 0;

			size_t index = 0;
			while(true) {
				assert(reuseQueue[index]->reuseIndex == index);

				size_t left = 2 * index + 1;
				size_t right = 2 * index + 2;
				if(left >= reuseQueue.size())
					break;

				size_t pivot = left;
				if(right < reuseQueue.size() && reuseQueue[right]->accessTime
						< reuseQueue[left]->accessTime)
					pivot = right;

				if(reuseQueue[index]->accessTime <= reuseQueue[pivot]->accessTime)
					break;

				reuseQueue[index]->reuseIndex = pivot;
				reuseQueue[pivot]->reuseIndex = index;
				swap(reuseQueue[index], reuseQueue[pivot]);
				index = pivot;
			}
		}

		// we can only reuse elements that are not locked
		if(element->lockCount != 0)
			continue;

		// remove previous identifier from cacheMap
		if(element->identifier) {
			auto mp = cacheMap.find(*element->identifier);
			assert(mp != cacheMap.end());
			cacheMap.erase(mp);
		}

		if(element->identifier)
			finishEntry(&element->entry);
		initEntry(identifier, &element->entry);

		// insert the new identifier into cacheMap
		element->identifier = identifier;
		auto inserted = cacheMap.insert(std::make_pair(identifier, element));
		assert(inserted.second);

		return Ref(this, element);
	}
}

// --------------------------------------------------------
// Cache::Ref
// --------------------------------------------------------

template<typename Ident, typename Entry>
Cache<Ident, Entry>::Ref::Ref()
: cache(nullptr), element(nullptr) { }

template<typename Ident, typename Entry>
Cache<Ident, Entry>::Ref::Ref(Ref &&other)
: Ref() {
	swap(*this, other);
}

template<typename Ident, typename Entry>
auto Cache<Ident, Entry>::Ref::operator= (Ref other) -> Ref & {
	swap(*this, other);
	return *this;
}

template<typename Ident, typename Entry>
Cache<Ident, Entry>::Ref::~Ref() {
	reset();
}

template<typename Ident, typename Entry>
void Cache<Ident, Entry>::Ref::reset() {
	if(!cache && !element)
		return;
	assert(cache && element);

	element->lockCount--;
	if(!element->lockCount) {
		element->accessTime = cache->currentTime++;
		
		// insert the element into reuseQueue
		if(element->reuseIndex == size_t(-1)) {
			element->reuseIndex = cache->reuseQueue.size();
			cache->reuseQueue.push_back(element);
		}
		assert(element->reuseIndex != size_t(-1));

		// repair the heap property
		size_t index = element->reuseIndex;
		while(index > 0) {
			assert(cache->reuseQueue[index]->reuseIndex == index);

			size_t parent = (index - 1) / 2;
			if(cache->reuseQueue[parent]->accessTime
					<= cache->reuseQueue[index]->accessTime)
				break;

			using std::swap;
			cache->reuseQueue[parent]->reuseIndex = index;
			cache->reuseQueue[index]->reuseIndex = parent;
			swap(cache->reuseQueue[index], cache->reuseQueue[parent]);
			index = parent;
		}
	}

	cache = nullptr;
	element = nullptr;
}

template<typename Ident, typename Entry>
void Cache<Ident, Entry>::Ref::swap(Ref &a, Ref &b) {
	using std::swap;
	swap(a.cache, b.cache);
	swap(a.element, b.element);
}

template<typename Ident, typename Entry>
Entry &Cache<Ident, Entry>::Ref::operator* () {
	return element->entry;
}

template<typename Ident, typename Entry>
Entry *Cache<Ident, Entry>::Ref::operator-> () {
	return  &element->entry;
}

template<typename Ident, typename Entry>
Cache<Ident, Entry>::Ref::Ref(Cache *cache, Element *element)
: cache(cache), element(element) {
	element->lockCount++;
}

} } // namespace blockfs::util

