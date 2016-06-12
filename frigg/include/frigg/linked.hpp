
#include <frigg/smart_ptr.hpp>

namespace frigg {

template<typename T>
struct IntrusiveSharedLinkedItem {
	SharedPtr<T> next;
	UnsafePtr<T> previous;
};

template<typename T, IntrusiveSharedLinkedItem<T> T::* Ptr>
struct IntrusiveSharedLinkedList {
	class Iterator {
	friend class IntrusiveSharedLinkedList;
	public:
		UnsafePtr<T> operator* () {
			return _current;
		}
		
		explicit operator bool() {
			return _current;
		}

		Iterator &operator++ () {
			_current = ((*_current).*Ptr).next;
			return *this;
		}

	private:
		Iterator(UnsafePtr<T> current)
		: _current(current) { }

		UnsafePtr<T> _current;
	};

	void addBack(SharedPtr<T> element) {
		// need a copy as we move out of the SharedPtr
		UnsafePtr<T> copy(element);
		if(!_back) {
			_front = move(element);
		}else{
			item(copy)->previous = _back;
			item(_back)->next = move(element);
		}
		_back = copy;
	}

	bool empty() {
		return !_front;
	}

	UnsafePtr<T> front() {
		return _front;
	}

	SharedPtr<T> removeFront() {
		return remove(frontIter());
	}

	SharedPtr<T> remove(const Iterator &it) {
		SharedPtr<T> next = move(item(it._current)->next);
		UnsafePtr<T> previous = item(it._current)->previous;

		if(!next) {
			_back = previous;
		}else{
			item(next)->previous = previous;
		}

		SharedPtr<T> erased;
		if(!previous) {
			erased = move(_front);
			_front = move(next);
		}else{
			erased = move(item(previous)->next);
			item(previous)->next = move(next);
		}

		item(it._current)->next = SharedPtr<T>();
		item(it._current)->previous = UnsafePtr<T>();

		assert(erased.get() == it._current.get());
		return erased;
	}

	Iterator frontIter() {
		return Iterator(_front);
	}

private:
	IntrusiveSharedLinkedItem<T> *item(UnsafePtr<T> ptr) {
		return &(ptr.get()->*Ptr);
	}

	SharedPtr<T> _front;
	UnsafePtr<T> _back;
};

template<typename T, typename Allocator>
class LinkedList {
private:
	struct Item {
		T element;
		Item *previous;
		Item *next;

		Item(const T &new_element) : element(new_element),
				previous(nullptr), next(nullptr) { }
		Item(T &&new_element) : element(move(new_element)),
				previous(nullptr), next(nullptr) { }
	};

public:
	class Iterator {
	friend class LinkedList;
	public:
		T &operator* () {
			return p_current->element;
		}
		T *operator-> () {
			return &p_current->element;
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

	void addBack(const T &element) {
		auto item = construct<Item>(p_allocator, element);
		addItemBack(item);
	}
	void addBack(T &&element) {
		auto item = construct<Item>(p_allocator, move(element));
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
		
		T element = move(item->element);

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

} // namespace frigg

