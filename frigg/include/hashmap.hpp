
namespace frigg {
namespace util {

template<typename Key, typename Value, typename Hasher, typename Allocator>
class Hashmap {
public:
	Hashmap(const Hasher &hasher, Allocator &allocator);
	~Hashmap();

	void insert(const Key &key, const Value &value);
	void insert(const Key &key, Value &&value);

	Value &get(const Key &key);

	Value remove(const Key &key);
	
	void rehash(size_t new_capacity);

private:
	struct Item {
		Key key;
		Value value;
		Item *chain;

		Item(const Key &new_key, const Value &new_value)
				: key(new_key), value(new_value), chain(nullptr) { }
		Item(const Key &new_key, Value &&new_value)
				: key(new_key), value(traits::move(new_value)), chain(nullptr) { }
	};
	
	Hasher p_hasher;
	Allocator &p_allocator;
	Item **p_table;
	size_t p_capacity;
	size_t p_size;
};

template<typename Key, typename Value, typename Hasher, typename Allocator>
Hashmap<Key, Value, Hasher, Allocator>::Hashmap(const Hasher &hasher, Allocator &allocator)
		: p_hasher(hasher), p_allocator(allocator), p_capacity(10), p_size(0) {
	p_table = (Item **)allocator.allocate(sizeof(Item *) * p_capacity);
	for(size_t i = 0; i < p_capacity; i++)
		p_table[i] = nullptr;
}

template<typename Key, typename Value, typename Hasher, typename Allocator>
Hashmap<Key, Value, Hasher, Allocator>::~Hashmap() {
	for(size_t i = 0; i < p_capacity; i++) {
		Item *item = p_table[i];
		while(item != nullptr) {
			Item *chain = item->chain;
			memory::destruct(p_allocator, item);
			item = chain;
		}
	}
	p_allocator.free(p_table);
}

template<typename Key, typename Value, typename Hasher, typename Allocator>
void Hashmap<Key, Value, Hasher, Allocator>::insert(const Key &key, const Value &value) {
	if(p_size > p_capacity)
		rehash(2 * p_size);

	unsigned int bucket = ((unsigned int)p_hasher(key)) % p_capacity;
	
	auto item = memory::construct<Item>(p_allocator, key, value);
	item->chain = p_table[bucket];
	p_table[bucket] = item;
	p_size++;
}
template<typename Key, typename Value, typename Hasher, typename Allocator>
void Hashmap<Key, Value, Hasher, Allocator>::insert(const Key &key, Value &&value) {
	if(p_size > p_capacity)
		rehash(2 * p_size);

	unsigned int bucket = ((unsigned int)p_hasher(key)) % p_capacity;
	
	auto item = memory::construct<Item>(p_allocator, key, traits::move(value));
	item->chain = p_table[bucket];
	p_table[bucket] = item;
	p_size++;
}

template<typename Key, typename Value, typename Hasher, typename Allocator>
Value &Hashmap<Key, Value, Hasher, Allocator>::get(const Key &key) {
	unsigned int bucket = ((unsigned int)p_hasher(key)) % p_capacity;

	for(Item *item = p_table[bucket]; item != nullptr; item = item->chain) {
		if(item->key == key)
			return item->value;
	}

	ASSERT(!"get(): Element not found");
	__builtin_unreachable();
}

template<typename Key, typename Value, typename Hasher, typename Allocator>
Value Hashmap<Key, Value, Hasher, Allocator>::remove(const Key &key) {
	unsigned int bucket = ((unsigned int)p_hasher(key)) % p_capacity;
	
	Item *previous = nullptr;
	for(Item *item = p_table[bucket]; item != nullptr; item = item->chain) {
		if(item->key == key) {
			Value value = traits::move(item->value);
			
			if(previous == nullptr) {
				p_table[bucket] = item->chain;
			}else{
				previous->chain = item->chain;
			}
			memory::destruct(p_allocator, item);
			p_size--;

			return value;
		}
	}

	ASSERT(!"remove(): Element not found");
	__builtin_unreachable();
}

template<typename Key, typename Value, typename Hasher, typename Allocator>
void Hashmap<Key, Value, Hasher, Allocator>::rehash(size_t new_capacity) {
	ASSERT(!"FIXME: Implement rehash");
}

template<typename T>
class DefaultHasher;

template<>
class DefaultHasher<uint64_t> {
public:
	unsigned int operator() (uint64_t v) {
		static_assert(sizeof(unsigned int) == 4, "Expected sizeof(int) == 4");
		return (unsigned int)(v ^ (v >> 32));
	}
};

class CStringHasher {
public:
	unsigned int operator() (const char *str) {
		unsigned int value = 0;
		while(*str != 0) {
			value = (value << 8) | (value >> 24);
			value += *str++;
		}
		return value;
	}
};

}} // namespace frigg::util

