#pragma once

// ----------------------------------------------------------------
// Range allocator.
// ----------------------------------------------------------------

#include <assert.h>
#include <limits.h>
#include <set>
#include <stddef.h>
#include <stdint.h>

struct range_allocator {
  private:
	struct node {
		uint64_t off;
		unsigned int ord;

		friend bool operator<(const node &u, const node &v) {
			if (u.ord != v.ord)
				return u.ord < v.ord;
			return u.off < v.off;
		}
	};

	static unsigned int clz(unsigned long x) { return __builtin_clzl(x); }

  public:
	static unsigned int round_order(size_t size) {
		assert(size >= 1);
		if (size == 1)
			return 0;
		return CHAR_BIT * sizeof(size_t) - clz(size - 1);
	}

	range_allocator(unsigned int order, unsigned int granularity) : _granularity{granularity} {
		_nodes.insert(node{0, order});
	}

	uint64_t allocate(size_t size) {
		return allocate_order(std::max(_granularity, round_order(size)));
	}

	uint64_t allocate_order(unsigned int order) {
		assert(order >= _granularity);

		auto it = _nodes.lower_bound(node{0, order});
		assert(it != _nodes.end());

		auto offset = it->off;

		while (it->ord != order) {
			assert(it->ord > order);
			auto high =
			    _nodes.insert(it, node{it->off + (uint64_t(1) << (it->ord - 1)), it->ord - 1});
			auto low = _nodes.insert(high, node{it->off, it->ord - 1});
			_nodes.erase(it);
			it = low;
		}
		_nodes.erase(it);

		return offset;
	}

	void free(uint64_t offset, size_t size) {
		return free_order(offset, std::max(_granularity, round_order(size)));
	}

	void free_order(uint64_t offset, unsigned int order) {
		assert(order >= _granularity);

		_nodes.insert(node{offset, order});
	}

  private:
	std::set<node> _nodes;
	unsigned int _granularity;
};
