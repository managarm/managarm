
#include <frigg/macros.hpp>

namespace frigg FRIGG_VISIBILITY {

template<typename T, typename Allocator>
class PriorityQueue {
public:
	PriorityQueue(Allocator &allocator)
	: p_heap(allocator) { }

	void enqueue(T &&event) {
		p_heap.push(move(event));
		up(p_heap.size() - 1);
	}

	T &front() {
		return p_heap[0];
	}

	T dequeue() {
		T event = move(p_heap[0]);
		
		swap(p_heap[0], p_heap[p_heap.size() - 1]);
		p_heap.pop();
		down(0);

		return event;
	}

	bool empty() {
		return p_heap.empty();
	}

private:
	static size_t left(size_t k) {
		return 2 * k + 1;
	}
	static size_t right(size_t k) {
		return 2 * k + 2;
	}
	static size_t parent(size_t k) {
		return k / 2;
	}

	void up(size_t k) {
		while(k != 0) {
			size_t p = parent(k);
			if(p_heap[p] < p_heap[k])
				break;

			swap(p_heap[p], p_heap[k]);
			k = p;
		}
	}
	
	void down(size_t k) {
		while(true) {
			size_t l = left(k), r = right(k);
			if(!(l < p_heap.size()))
				break;

			size_t c = l;
			if(r < p_heap.size())
				if(p_heap[r] < p_heap[l])
					c = r;

			if(p_heap[k] < p_heap[c])
				break;

			swap(p_heap[k], p_heap[c]);
			k = c;
		}
	}

	Vector<T, Allocator> p_heap;
};

} // namespace frigg

