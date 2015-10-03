
#ifndef FRIGG_HASHMAP_HPP
#define FRIGG_HASHMAP_HPP

#include <frigg/tuple.hpp>
#include <frigg/optional.hpp>

namespace frigg {

template<typename Key, typename Value, typename Hasher, typename Allocator>
class Hashmap {
public:
	typedef Tuple<const Key, Value> Entry;

private:
	struct Item {
		Entry entry;
		Item *chain;

		Item(const Key &new_key, const Value &new_value)
		: entry(new_key, new_value), chain(nullptr) { }

		Item(const Key &new_key, Value &&new_value)
		: entry(new_key, move(new_value)), chain(nullptr) { }
	};

public:
	class Iterator {
	friend class Hashmap;
	public:
		Iterator &operator++ () {
			assert(item);
			item = item->chain;
			if(item)
				return *this;

			while(bucket < map.p_capacity) {
				item = map.p_table[bucket];
				bucket++;
				if(item)
					break;
			}

			return *this;
		}

		Entry &operator* () {
			return item->entry;
		}
		Entry *operator-> () {
			return &item->entry;
		}

		operator bool () {
			return item != nullptr;
		}

	private:
		Iterator(Hashmap &map, size_t bucket, Item *item)
		: map(map), item(item), bucket(bucket) { }

		Hashmap &map;
		Item *item;
		size_t bucket;
	};

	Hashmap(const Hasher &hasher, Allocator &allocator);
	~Hashmap();

	void insert(const Key &key, const Value &value);
	void insert(const Key &key, Value &&value);

	Iterator iterator() {
		if(!p_size)
			return Iterator(*this, p_capacity, nullptr);

		for(size_t bucket = 0; bucket < p_capacity; bucket++) {
			if(p_table[bucket])
				return Iterator(*this, bucket, p_table[bucket]);
		}
		
		assert(!"Hashmap corrupted");
		__builtin_unreachable();
	}
	
	template<typename KeyCompatible>
	Value *get(const KeyCompatible &key);

	Optional<Value> remove(const Key &key);

private:
	void rehash();
	
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
			destruct(p_allocator, item);
			item = chain;
		}
	}
	p_allocator.free(p_table);
}

template<typename Key, typename Value, typename Hasher, typename Allocator>
void Hashmap<Key, Value, Hasher, Allocator>::insert(const Key &key, const Value &value) {
	if(p_size > p_capacity)
		rehash();

	unsigned int bucket = ((unsigned int)p_hasher(key)) % p_capacity;
	
	auto item = construct<Item>(p_allocator, key, value);
	item->chain = p_table[bucket];
	p_table[bucket] = item;
	p_size++;
}
template<typename Key, typename Value, typename Hasher, typename Allocator>
void Hashmap<Key, Value, Hasher, Allocator>::insert(const Key &key, Value &&value) {
	if(p_size > p_capacity)
		rehash();

	unsigned int bucket = ((unsigned int)p_hasher(key)) % p_capacity;
	
	auto item = construct<Item>(p_allocator, key, move(value));
	item->chain = p_table[bucket];
	p_table[bucket] = item;
	p_size++;
}

template<typename Key, typename Value, typename Hasher, typename Allocator>
template<typename KeyCompatible>
Value *Hashmap<Key, Value, Hasher, Allocator>::get(const KeyCompatible &key) {
	unsigned int bucket = ((unsigned int)p_hasher(key)) % p_capacity;

	for(Item *item = p_table[bucket]; item != nullptr; item = item->chain) {
		if(item->entry.template get<0>() == key)
			return &item->entry.template get<1>();
	}

	return nullptr;
}

template<typename Key, typename Value, typename Hasher, typename Allocator>
Optional<Value> Hashmap<Key, Value, Hasher, Allocator>::remove(const Key &key) {
	unsigned int bucket = ((unsigned int)p_hasher(key)) % p_capacity;
	
	Item *previous = nullptr;
	for(Item *item = p_table[bucket]; item != nullptr; item = item->chain) {
		if(item->entry.template get<0>() == key) {
			Value value = move(item->entry.template get<1>());
			
			if(previous == nullptr) {
				p_table[bucket] = item->chain;
			}else{
				previous->chain = item->chain;
			}
			destruct(p_allocator, item);
			p_size--;

			return value;
		}

		previous = item;
	}

	return Optional<Value>();
}

template<typename Key, typename Value, typename Hasher, typename Allocator>
void Hashmap<Key, Value, Hasher, Allocator>::rehash() {
	size_t new_capacity = 2 * p_size;
	Item **new_table = (Item **)p_allocator.allocate(sizeof(Item *) * new_capacity);
	for(size_t i = 0; i < new_capacity; i++)
		new_table[i] = nullptr;
	
	for(size_t i = 0; i < p_capacity; i++) {
		Item *item = p_table[i];
		while(item != nullptr) {
			auto bucket = ((unsigned int)p_hasher(item->entry.template get<0>())) % new_capacity;

			Item *chain = item->chain;
			item->chain = new_table[bucket];
			new_table[bucket] = item;
			item = chain;
		}
	}

	p_allocator.free(p_table);
	p_table = new_table;
	p_capacity = new_capacity;
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

template<>
class DefaultHasher<int> {
public:
	unsigned int operator() (int v) {
		return v;
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

} // namespace frigg

#endif // FRIGG_HASHMAP_HPP

