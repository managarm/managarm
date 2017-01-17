
#ifndef FRIGG_RBTREE_HPP
#define FRIGG_RBTREE_HPP

#include <frigg/macros.hpp>

namespace frigg FRIGG_VISIBILITY {

namespace _redblack {

enum class color_type {
	null, red, black
};

struct hook_struct {
	hook_struct()
	: parent(nullptr), left(nullptr), right(nullptr),
			predecessor(nullptr), successor(nullptr),
			color(color_type::null) { }

	hook_struct(const hook_struct &other) = delete;

	hook_struct &operator= (const hook_struct &other) = delete;

	void *parent;
	void *left;
	void *right;
	void *predecessor;
	void *successor;
	color_type color;
};

template<typename T, hook_struct T:: *Member, typename L, typename A>
struct tree_struct {
private:
	// obtain a pointer to the hook from a pointer to a node.
	static hook_struct *h(T *item) {
		return &(item->*Member);
	}

public:
	static T *get_parent(T *item) {
		return static_cast<T *>(h(item)->parent);
	}

	// TODO: rename to left()/right()?
	static T *get_left(T *item) {
		return static_cast<T *>(h(item)->left);
	}
	static T *get_right(T *item) {
		return static_cast<T *>(h(item)->right);
	}

	static T *predecessor(T *item) {
		return static_cast<T *>(h(item)->predecessor);
	}
	static T *successor(T *item) {
		return static_cast<T *>(h(item)->successor);
	}

	T *get_root() {
		return static_cast<T *>(_root);
	}

private:
	static bool isRed(T *node) {
		if(!node)
			return false;
		return h(node)->color == color_type::red;
	}
	static bool isBlack(T *node) {
		if(!node)
			return true;
		return h(node)->color == color_type::black;
	}
	
	// ------------------------------------------------------------------------
	// Constructor, Destructor, operators.
	// ------------------------------------------------------------------------
public:
	tree_struct(L less = L())
	: _less(move(less)), _root(nullptr) { }

	tree_struct(const tree_struct &other) = delete;

	tree_struct &operator= (const tree_struct &other) = delete;
	
	// ------------------------------------------------------------------------
	// Traversal functions.
	// ------------------------------------------------------------------------

	T *first() {
		T *current = get_root();
		assert(current);
		while(get_left(current))
			current = get_left(current);
		return current;
	}

	// ------------------------------------------------------------------------
	// Insertion functions.
	// ------------------------------------------------------------------------
public:
	void insert(T *node) {
		assert(check_invariant());

		if(!_root) {
			_root = node;

			fix_insert(node);
			assert(check_invariant());
			return;
		}

		T *current = get_root();
		while(true) {
			if(_less(*node, *current)) {
				if(get_left(current) == nullptr) {
					h(current)->left = node;
					h(node)->parent = current;

					// "current" is the successor of "node"
					T *pred = predecessor(current);
					if(pred)
						h(pred)->successor = node;
					h(node)->predecessor = pred;
					h(node)->successor = current;
					h(current)->predecessor = node;

					aggregate_path(current);

					fix_insert(node);
					assert(check_invariant());
					return;
				}else{
					current = get_left(current);
				}
			}else{
				if(get_right(current) == nullptr) {
					h(current)->right = node;
					h(node)->parent = current;

					// "current" is the predecessor of "node"
					T *succ = successor(current);
					h(current)->successor = node;
					h(node)->predecessor = current;
					h(node)->successor = succ;
					if(succ)
						h(succ)->predecessor = node;
					
					aggregate_path(current);
					
					fix_insert(node);
					assert(check_invariant());
					return;
				}else{
					current = get_right(current);
				}
			}
		}
	}

	// Situation:
	// |     (p)     |
	// |    /   \    |
	// |  (s)   (n)  |
	// Precondition: The red-black property is only violated in the following sense:
	//     Paths from (p) over (n) to a leaf contain one black node more
	//     than paths from (p) over (s) to a leaf.
	//     n itself might be either red or black.
	// Postcondition: The red-black property is satisfied.
private:
	void fix_insert(T *n) {
		T *parent = get_parent(n);
		if(parent == nullptr) {
			h(n)->color = color_type::black;
			return;
		}
		
		// coloring n red is not a problem if the parent is black.
		h(n)->color = color_type::red;
		if(h(parent)->color == color_type::black)
			return;
		
		// the rb invariants guarantee that a grandparent exists
		// (because parent is red and the root is black).
		T *grand = get_parent(parent);
		assert(grand && h(grand)->color == color_type::black);
		
		// if the node has a red uncle we can just color both the parent
		// and the uncle black, the grandparent red and propagate upwards.
		if(get_left(grand) == parent && isRed(get_right(grand))) {
			h(grand)->color = color_type::red;
			h(parent)->color = color_type::black;
			h(get_right(grand))->color = color_type::black;

			fix_insert(grand);
			return;
		}else if(get_right(grand) == parent && isRed(get_left(grand))) {
			h(grand)->color = color_type::red;
			h(parent)->color = color_type::black;
			h(get_left(grand))->color = color_type::black;

			fix_insert(grand);
			return;
		}
		
		if(parent == get_left(grand)) {
			if(n == get_right(parent)) {
				rotateLeft(n);
				rotateRight(n);
				h(n)->color = color_type::black;
			}else{
				rotateRight(parent);
				h(parent)->color = color_type::black;
			}
			h(grand)->color = color_type::red;
		}else{
			assert(parent == get_right(grand));
			if(n == get_left(parent)) {
				rotateRight(n);
				rotateLeft(n);
				h(n)->color = color_type::black;
			}else{
				rotateLeft(parent);
				h(parent)->color = color_type::black;
			}
			h(grand)->color = color_type::red;
		}
	}
	
	// ------------------------------------------------------------------------
	// Removal functions.
	// ------------------------------------------------------------------------
public:
	void remove(T *mapping) {
		assert(check_invariant());

		T *left_ptr = get_left(mapping);
		T *right_ptr = get_right(mapping);

		if(!left_ptr) {
			remove_half_leaf(mapping, right_ptr);
		}else if(!right_ptr) {
			remove_half_leaf(mapping, left_ptr);
		}else{
			// replace the mapping by its predecessor
			T *pred = predecessor(mapping);
			remove_half_leaf(pred, get_left(pred));
			replace_node(mapping, pred);
		}
		
		assert(check_invariant());
	}

private:
	void replace_node(T *node, T *replacement) {
		T *parent = get_parent(node);
		T *left = get_left(node);
		T *right = get_right(node);

		// fix the red-black tree
		if(parent == nullptr) {
			_root = replacement;
		}else if(node == get_left(parent)) {
			h(parent)->left = replacement;
		}else{
			assert(node == get_right(parent));
			h(parent)->right = replacement;
		}
		h(replacement)->parent = parent;
		h(replacement)->color = h(node)->color;

		h(replacement)->left = left;
		if(left)
			h(left)->parent = replacement;
		
		h(replacement)->right = right;
		if(right)
			h(right)->parent = replacement;
		
		// fix the linked list
		if(predecessor(node))
			h(predecessor(node))->successor = replacement;
		h(replacement)->predecessor = predecessor(node);
		h(replacement)->successor = successor(node);
		if(successor(node))
			h(successor(node))->predecessor = replacement;
		
		h(node)->left = nullptr;
		h(node)->right = nullptr;
		h(node)->parent = nullptr;
		h(node)->predecessor = nullptr;
		h(node)->successor = nullptr;
		
		aggregate_node(replacement);
		aggregate_path(parent);
	}

	void remove_half_leaf(T *mapping, T *child) {
		T *pred = predecessor(mapping);
		T *succ = successor(mapping);
		if(pred)
			h(pred)->successor = succ;
		if(succ)
			h(succ)->predecessor = pred;

		if(h(mapping)->color == color_type::black) {
			if(isRed(child)) {
				h(child)->color = color_type::black;
			}else{
				// decrement the number of black nodes all paths through "mapping"
				// before removing the child. this makes sure we're correct even when
				// "child" is null
				fix_remove(mapping);
			}
		}
		
		assert((!get_left(mapping) && get_right(mapping) == child)
				|| (get_left(mapping) == child && !get_right(mapping)));
			
		T *parent = get_parent(mapping);
		if(!parent) {
			_root = child;
		}else if(get_left(parent) == mapping) {
			h(parent)->left = child;
		}else{
			assert(get_right(parent) == mapping);
			h(parent)->right = child;
		}
		if(child)
			h(child)->parent = parent;
		
		h(mapping)->left = nullptr;
		h(mapping)->right = nullptr;
		h(mapping)->parent = nullptr;
		h(mapping)->predecessor = nullptr;
		h(mapping)->successor = nullptr;
		
		if(parent)
			aggregate_path(parent);
	}

	// Situation:
	// |     (p)     |
	// |    /   \    |
	// |  (s)   (n)  |
	// Precondition: The red-black property is only violated in the following sense:
	//     Paths from (p) over (n) to a leaf contain one black node less
	//     than paths from (p) over (s) to a leaf
	// Postcondition: The whole tree is a red-black tree
	void fix_remove(T *n) {
		assert(h(n)->color == color_type::black);
		
		T *parent = get_parent(n);
		if(parent == nullptr)
			return;
		
		// rotate so that our node has a black sibling
		T *s; // this will always be the sibling of our node
		if(get_left(parent) == n) {
			assert(get_right(parent));
			if(h(get_right(parent))->color == color_type::red) {
				T *x = get_right(parent);
				rotateLeft(get_right(parent));
				assert(n == get_left(parent));
				
				h(parent)->color = color_type::red;
				h(x)->color = color_type::black;
			}
			
			s = get_right(parent);
		}else{
			assert(get_right(parent) == n);
			assert(get_left(parent));
			if(h(get_left(parent))->color == color_type::red) {
				T *x = get_left(parent);
				rotateRight(x);
				assert(n == get_right(parent));
				
				h(parent)->color = color_type::red;
				h(x)->color = color_type::black;
			}
			
			s = get_left(parent);
		}
		
		if(isBlack(get_left(s)) && isBlack(get_right(s))) {
			if(h(parent)->color == color_type::black) {
				h(s)->color = color_type::red;
				fix_remove(parent);
				return;
			}else{
				h(parent)->color = color_type::black;
				h(s)->color = color_type::red;
				return;
			}
		}
		
		// now at least one of s children is red
		auto parent_color = h(parent)->color;
		if(get_left(parent) == n) {
			// rotate so that get_right(s) is red
			if(isRed(get_left(s)) && isBlack(get_right(s))) {
				T *child = get_left(s);
				rotateRight(child);

				h(s)->color = color_type::red;
				h(child)->color = color_type::black;

				s = child;
			}
			assert(isRed(get_right(s)));

			rotateLeft(s);
			h(parent)->color = color_type::black;
			h(s)->color = parent_color;
			h(get_right(s))->color = color_type::black;
		}else{
			assert(get_right(parent) == n);

			// rotate so that get_left(s) is red
			if(isRed(get_right(s)) && isBlack(get_left(s))) {
				T *child = get_right(s);
				rotateLeft(child);

				h(s)->color = color_type::red;
				h(child)->color = color_type::black;

				s = child;
			}
			assert(isRed(get_left(s)));

			rotateRight(s);
			h(parent)->color = color_type::black;
			h(s)->color = parent_color;
			h(get_left(s))->color = color_type::black;
		}
	}
	
	// ------------------------------------------------------------------------
	// Rotation functions.
	// ------------------------------------------------------------------------
private:
	// Left rotation (n denotes the given mapping):
	//   w                 w        |
	//   |                 |        |
	//   u                 n        |
	//  / \      -->      / \       |
	// x   n             u   y      |
	//    / \           / \         |
	//   v   y         x   v        |
	// Note that x and y are left unchanged.
	void rotateLeft(T *n) {
		T *u = get_parent(n);
		assert(u != nullptr && get_right(u) == n);
		T *v = get_left(n);
		T *w = get_parent(u);

		if(v != nullptr)
			h(v)->parent = u;
		h(u)->right = v;
		h(u)->parent = n;
		h(n)->left = u;
		h(n)->parent = w;

		if(w == nullptr) {
			_root = n;
		}else if(get_left(w) == u) {
			h(w)->left = n;
		}else{
			assert(get_right(w) == u);
			h(w)->right = n;
		}

		aggregate_node(u);
		aggregate_node(n);
	}

	// Right rotation (n denotes the given mapping):
	//     w             w          |
	//     |             |          |
	//     u             n          |
	//    / \    -->    / \         |
	//   n   x         y   u        |
	//  / \               / \       |
	// y   v             v   x      |
	// Note that x and y are left unchanged.
	void rotateRight(T *n) {
		T *u = get_parent(n);
		assert(u != nullptr && get_left(u) == n);
		T *v = get_right(n);
		T *w = get_parent(u);
		
		if(v != nullptr)
			h(v)->parent = u;
		h(u)->left = v;
		h(u)->parent = n;
		h(n)->right = u;
		h(n)->parent = w;

		if(w == nullptr) {
			_root = n;
		}else if(get_left(w) == u) {
			h(w)->left = n;
		}else{
			assert(get_right(w) == u);
			h(w)->right = n;
		}

		aggregate_node(u);
		aggregate_node(n);
	}
	
	// ------------------------------------------------------------------------
	// Aggregation functions.
	// ------------------------------------------------------------------------
public:
	void aggregate_node(T *node) {
		A::aggregate(node);
	}

	void aggregate_path(T *node) {
		T *current = node;
		while(current) {
			if(!A::aggregate(current))
				break;
			current = get_parent(current);
		}
	}

	// ------------------------------------------------------------------------
	// Invariant validation functions.
	// ------------------------------------------------------------------------
private:
	bool check_invariant() {
		if(!_root)
			return true;

		int black_depth;
		T *minimal, *maximal;
		return check_invariant(get_root(), black_depth, minimal, maximal);
	}

	bool check_invariant(T *node, int &black_depth, T *&minimal, T *&maximal) {
		// check alternating colors invariant
		if(h(node)->color == color_type::red)
			if(!isBlack(get_left(node)) || !isBlack(get_right(node))) {
				infoLogger() << "Alternating colors violation" << endLog;
				return false;
			}
		
		// check recursive invariants
		int left_black_depth = 0;
		int right_black_depth = 0;
		
		if(get_left(node)) {
			if(_less(*node, *get_left(node))) {
				infoLogger() << "Binary search tree (left) violation" << endLog;
				return false;
			}

			T *pred;
			if(!check_invariant(get_left(node), left_black_depth, minimal, pred))
				return false;

			// check predecessor invariant
			if(successor(pred) != node) {
				infoLogger() << "Linked list (predecessor, forward) violation" << endLog;
				return false;
			}else if(predecessor(node) != pred) {
				infoLogger() << "Linked list (predecessor, backward) violation" << endLog;
				return false;
			}
		}else{
			minimal = node;
		}

		if(get_right(node)) {
			if(_less(*get_right(node), *node)) {
				infoLogger() << "Binary search tree (right) violation" << endLog;
				return false;
			}

			T *succ;
			if(!check_invariant(get_right(node), right_black_depth, succ, maximal))
				return false;
			
			// check successor invariant
			if(successor(node) != succ) {
				infoLogger() << "Linked list (successor, forward) violation" << endLog;
				return false;
			}else if(predecessor(succ) != node) {
				infoLogger() << "Linked list (successor, backward) violation" << endLog;
				return false;
			}
		}else{
			maximal = node;
		}
		
		// check black-depth invariant
		if(left_black_depth != right_black_depth) {
			infoLogger() << "Black-depth violation" << endLog;
			return false;
		}
		black_depth = left_black_depth;
		if(h(node)->color == color_type::black)
			black_depth++;

		if(!A::check_invariant(*this, node))
			return false;

		return true;
	}

private:
	L _less;
	void *_root;
};

} // namespace _redblack

using rbtree_hook = _redblack::hook_struct;

template<typename T, rbtree_hook T:: *Member, typename A, typename L>
using rbtree = _redblack::tree_struct<T, Member, A, L>;

} // namespace frigg

#endif // FRIGG_RBTREE_HPP

