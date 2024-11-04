#pragma once

// ----------------------------------------------------------------
// Sequential ID allocator
// ----------------------------------------------------------------

#include <assert.h>
#include <limits>
#include <set>

// Allocator for integral IDs. Provides O(log n) allocation and deallocation.
// Allocation always returns the smallest available ID.
template <typename T> struct id_allocator {
  private:
	struct node {
		T lb;
		T ub;

		friend bool operator<(const node &u, const node &v) { return u.lb < v.lb; }
	};

  public:
	id_allocator(T lb = 1, T ub = std::numeric_limits<T>::max()) { _nodes.insert(node{lb, ub}); }

	T allocate() {
		assert(!_nodes.empty());
		auto it = _nodes.begin();
		auto id = it->lb;
		if (it->lb < it->ub)
			_nodes.insert(std::next(it), node{it->lb + 1, it->ub});
		_nodes.erase(it);
		return id;
	}

	void free(T id) {
		// TODO: We could coalesce multiple adjacent nodes here.
		_nodes.insert(node{id, id});
	}

  private:
	std::set<node> _nodes;
};
