
namespace thor {
namespace util {

template<typename T, typename Allocator>
class LinkedList {
private:
	struct Item {
		T element;
		Item *previous;
		Item *next;

		Item(const T &new_element) : element(new_element),
				next(nullptr), previous(nullptr) { }
		Item(T &&new_element) : element(util::move(new_element)),
				next(nullptr), previous(nullptr) { }
	};

public:
	class Iterator {
	friend class LinkedList;
	public:
		T &operator* () {
			return p_current->element;
		}

		Iterator &operator++ () {
			p_current = p_current->next;
			return *this;
		}
		
		bool okay() {
			return p_current != nullptr;
		}

	private:
		Iterator(Item *item) : p_current(item) { }

		Item *p_current;
	};

	LinkedList(Allocator &allocator)
			: p_allocator(allocator), p_front(nullptr), p_back(nullptr) { }
	~LinkedList() {
		Item *item = p_front;
		while(item != nullptr) {
			Item *next = item->next;
			destruct(p_allocator, next);
			item = next;
		}
	}

	void addBack(T &&element) {
		auto item = construct<Item>(p_allocator, util::move(element));
		addItemBack(item);
	}

	bool empty() {
		return p_front == nullptr;
	}

	T &front() {
		return p_front->element;
	}

	T removeFront() {
		return remove(frontIter());
	}

	T remove(const Iterator &iter) {
		Item *item = iter.p_current;
		
		T element = util::move(item->element);

		Item *next = item->next;
		Item *previous = item->previous;
		destruct(p_allocator, item);

		if(next == nullptr) {
			p_back = previous;
		}else{
			next->previous = previous;
		}

		if(previous == nullptr) {
			p_front = next;
		}else{
			previous->next = next;
		}

		return element;
	}

	Iterator frontIter() {
		return Iterator(p_front);
	}

private:
	void addItemBack(Item *item) {
		if(p_back == nullptr) {
			p_front = item;
		}else{
			item->previous = p_back;
			p_back->next = item;
		}
		p_back = item;
	}
	
	Allocator &p_allocator;
	Item *p_front;
	Item *p_back;
};

} } // namespace thor::util

