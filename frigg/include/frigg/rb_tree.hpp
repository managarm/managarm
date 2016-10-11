
#ifndef FRIGG_RB_TREE_HPP
#define FRIGG_RB_TREE_HPP

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

	void *parent;
	void *left;
	void *right;
	void *predecessor;
	void *successor;
	color_type color;
};

template<typename T, hook_struct T:: *Member, typename L, typename A>
struct tree_struct {
	static T *get_parent(T *item) {
		return static_cast<T *>((item->*Member).parent);
	}

	// TODO: rename to left()/right()?
	static T *get_left(T *item) {
		return static_cast<T *>((item->*Member).left);
	}
	static T *get_right(T *item) {
		return static_cast<T *>((item->*Member).right);
	}

	static T *predecessor(T *item) {
		return static_cast<T *>((item->*Member).predecessor);
	}
	static T *successor(T *item) {
		return static_cast<T *>((item->*Member).successor);
	}

	T *get_root() {
		return static_cast<T *>(_root);
	}

private:
	static bool isRed(T *node) {
		if(!node)
			return false;
		return (node->*Member).color == color_type::red;
	}
	static bool isBlack(T *node) {
		if(!node)
			return true;
		return (node->*Member).color == color_type::black;
	}
	
	// ------------------------------------------------------------------------
	// Constructor, Destructor, operators.
	// ------------------------------------------------------------------------
public:
	tree_struct(L less = L())
	: _less(move(less)), _root(nullptr) { }

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
					(current->*Member).left = node;
					(node->*Member).parent = current;

					// "current" is the successor of "node"
					T *pred = predecessor(current);
					if(pred)
						(pred->*Member).successor = node;
					(node->*Member).predecessor = pred;
					(node->*Member).successor = current;
					(current->*Member).predecessor = node;

					aggregate_path(current);

					fix_insert(node);
					assert(check_invariant());
					return;
				}else{
					current = get_left(current);
				}
			}else{
				if(get_right(current) == nullptr) {
					(current->*Member).right = node;
					(node->*Member).parent = current;

					// "current" is the predecessor of "node"
					T *succ = successor(current);
					(current->*Member).successor = node;
					(node->*Member).predecessor = current;
					(node->*Member).successor = succ;
					if(succ)
						(succ->*Member).predecessor = node;
					
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
			(n->*Member).color = color_type::black;
			return;
		}
		
		// coloring n red is not a problem if the parent is black.
		(n->*Member).color = color_type::red;
		if((parent->*Member).color == color_type::black)
			return;
		
		// the rb invariants guarantee that a grandparent exists
		// (because parent is red and the root is black).
		T *grand = get_parent(parent);
		assert(grand && (grand->*Member).color == color_type::black);
		
		// if the node has a red uncle we can just color both the parent
		// and the uncle black, the grandparent red and propagate upwards.
		if(get_left(grand) == parent && isRed(get_right(grand))) {
			(grand->*Member).color = color_type::red;
			(parent->*Member).color = color_type::black;
			(get_right(grand)->*Member).color = color_type::black;

			fix_insert(grand);
			return;
		}else if(get_right(grand) == parent && isRed(get_left(grand))) {
			(grand->*Member).color = color_type::red;
			(parent->*Member).color = color_type::black;
			(get_left(grand)->*Member).color = color_type::black;

			fix_insert(grand);
			return;
		}
		
		if(parent == get_left(grand)) {
			if(n == get_right(parent)) {
				rotateLeft(n);
				rotateRight(n);
				(n->*Member).color = color_type::black;
			}else{
				rotateRight(parent);
				(parent->*Member).color = color_type::black;
			}
			(grand->*Member).color = color_type::red;
		}else{
			assert(parent == get_right(grand));
			if(n == get_left(parent)) {
				rotateRight(n);
				rotateLeft(n);
				(n->*Member).color = color_type::black;
			}else{
				rotateLeft(parent);
				(parent->*Member).color = color_type::black;
			}
			(grand->*Member).color = color_type::red;
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
			(parent->*Member).left = replacement;
		}else{
			assert(node == get_right(parent));
			(parent->*Member).right = replacement;
		}
		(replacement->*Member).parent = parent;
		(replacement->*Member).color = (node->*Member).color;

		(replacement->*Member).left = left;
		if(left)
			(left->*Member).parent = replacement;
		
		(replacement->*Member).right = right;
		if(right)
			(right->*Member).parent = replacement;
		
		// fix the linked list
		if(predecessor(node))
			(predecessor(node)->*Member).successor = replacement;
		(replacement->*Member).predecessor = predecessor(node);
		(replacement->*Member).successor = successor(node);
		if(successor(node))
			(successor(node)->*Member).predecessor = replacement;
		
		(node->*Member).left = nullptr;
		(node->*Member).right = nullptr;
		(node->*Member).parent = nullptr;
		(node->*Member).predecessor = nullptr;
		(node->*Member).successor = nullptr;
		
		aggregate_node(replacement);
		aggregate_path(parent);
	}

	void remove_half_leaf(T *mapping, T *child) {
		T *pred = predecessor(mapping);
		T *succ = successor(mapping);
		if(pred)
			(pred->*Member).successor = succ;
		if(succ)
			(succ->*Member).predecessor = pred;

		if((mapping->*Member).color == color_type::black) {
			if(isRed(child)) {
				(child->*Member).color = color_type::black;
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
			(parent->*Member).left = child;
		}else{
			assert(get_right(parent) == mapping);
			(parent->*Member).right = child;
		}
		if(child)
			(child->*Member).parent = parent;
		
		(mapping->*Member).left = nullptr;
		(mapping->*Member).right = nullptr;
		(mapping->*Member).parent = nullptr;
		(mapping->*Member).predecessor = nullptr;
		(mapping->*Member).successor = nullptr;
		
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
		assert((n->*Member).color == color_type::black);
		
		T *parent = get_parent(n);
		if(parent == nullptr)
			return;
		
		// rotate so that our node has a black sibling
		T *s; // this will always be the sibling of our node
		if(get_left(parent) == n) {
			assert(get_right(parent));
			if((get_right(parent)->*Member).color == color_type::red) {
				T *x = get_right(parent);
				rotateLeft(get_right(parent));
				assert(n == get_left(parent));
				
				(parent->*Member).color = color_type::red;
				(x->*Member).color = color_type::black;
			}
			
			s = get_right(parent);
		}else{
			assert(get_right(parent) == n);
			assert(get_left(parent));
			if((get_left(parent)->*Member).color == color_type::red) {
				T *x = get_left(parent);
				rotateRight(x);
				assert(n == get_right(parent));
				
				(parent->*Member).color = color_type::red;
				(x->*Member).color = color_type::black;
			}
			
			s = get_left(parent);
		}
		
		if(isBlack(get_left(s)) && isBlack(get_right(s))) {
			if((parent->*Member).color == color_type::black) {
				(s->*Member).color = color_type::red;
				fix_remove(parent);
				return;
			}else{
				(parent->*Member).color = color_type::black;
				(s->*Member).color = color_type::red;
				return;
			}
		}
		
		// now at least one of s children is red
		auto parent_color = (parent->*Member).color;
		if(get_left(parent) == n) {
			// rotate so that get_right(s) is red
			if(isRed(get_left(s)) && isBlack(get_right(s))) {
				T *child = get_left(s);
				rotateRight(child);

				(s->*Member).color = color_type::red;
				(child->*Member).color = color_type::black;

				s = child;
			}
			assert(isRed(get_right(s)));

			rotateLeft(s);
			(parent->*Member).color = color_type::black;
			(s->*Member).color = parent_color;
			(get_right(s)->*Member).color = color_type::black;
		}else{
			assert(get_right(parent) == n);

			// rotate so that get_left(s) is red
			if(isRed(get_right(s)) && isBlack(get_left(s))) {
				T *child = get_right(s);
				rotateLeft(child);

				(s->*Member).color = color_type::red;
				(child->*Member).color = color_type::black;

				s = child;
			}
			assert(isRed(get_left(s)));

			rotateRight(s);
			(parent->*Member).color = color_type::black;
			(s->*Member).color = parent_color;
			(get_left(s)->*Member).color = color_type::black;
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
			(v->*Member).parent = u;
		(u->*Member).right = v;
		(u->*Member).parent = n;
		(n->*Member).left = u;
		(n->*Member).parent = w;

		if(w == nullptr) {
			_root = n;
		}else if(get_left(w) == u) {
			(w->*Member).left = n;
		}else{
			assert(get_right(w) == u);
			(w->*Member).right = n;
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
			(v->*Member).parent = u;
		(u->*Member).left = v;
		(u->*Member).parent = n;
		(n->*Member).right = u;
		(n->*Member).parent = w;

		if(w == nullptr) {
			_root = n;
		}else if(get_left(w) == u) {
			(w->*Member).left = n;
		}else{
			assert(get_right(w) == u);
			(w->*Member).right = n;
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
		if((node->*Member).color == color_type::red)
			if(!isBlack(get_left(node)) || !isBlack(get_right(node))) {
				infoLogger() << "Alternating colors violation" << endLog;
				infoLogger() << "Red node has childen with "
						<< (int)(get_left(node)->*Member).color
						<< " and " << (int)(get_right(node)->*Member).color << endLog;
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
		if((node->*Member).color == color_type::black)
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

using rb_hook = _redblack::hook_struct;

template<typename T, rb_hook T:: *Member, typename A, typename L>
using rb_tree = _redblack::tree_struct<T, Member, A, L>;

} // namespace frigg

#endif // FRIGG_RB_TREE_HPP

