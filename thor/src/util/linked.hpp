
namespace thor {
namespace util {

template<typename T, typename Allocator>
class LinkedList {
public:
	LinkedList(Allocator *allocator)
			: p_allocator(allocator), p_front(nullptr), p_back(nullptr) { }

	void addBack(T &&element) {
		Item *item = new (p_allocator) Item(util::move(element));
		addItemBack(item);
	}

	bool empty() {
		return p_front == nullptr;
	}

	T removeFront() {
		Item *front = p_front;
		if(front == nullptr) {
			debug::criticalLogger->log("LinkedList::removeFront(): List is empty!");
			debug::panic();
		}

		T element = util::move(front->element);

		Item *next = front->next;
		if(next == nullptr) {
			p_back = nullptr;
		}else{
			next->previous = nullptr;
		}
		p_front = next;

		return element;
	}

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

	void addItemBack(Item *item) {
		if(p_back == nullptr) {
			p_front = item;
		}else{
			item->previous = p_back;
			p_back->next = item;
		}
		p_back = item;
	}
	
	Allocator *p_allocator;
	Item *p_front;
	Item *p_back;
};

} } // namespace thor::util

