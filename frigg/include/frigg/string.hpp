
#ifndef FRIGG_STRING_HPP
#define FRIGG_STRING_HPP

#include <frigg/macros.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/optional.hpp>
#include <frigg/debug.hpp>

namespace frigg FRIGG_VISIBILITY {

template<typename Char>
class BasicStringView {
public:
	typedef Char CharType;

	BasicStringView()
	: p_pointer(nullptr), p_length(0) { }

	BasicStringView(const Char *c_string)
	: p_pointer(c_string), p_length(strlen(c_string)) { }

	BasicStringView(const Char *pointer, size_t length)
	: p_pointer(pointer), p_length(length) { }

	const Char *data() const {
		return p_pointer;
	}

	const Char &operator[] (size_t index) const {
		return p_pointer[index];
	}

	size_t size() const {
		return p_length;
	}

	bool operator== (BasicStringView other) {
		if(p_length != other.p_length)
			return false;
		for(size_t i = 0; i < p_length; i++)
			if(p_pointer[i] != other.p_pointer[i])
				return false;
		return true;
	}
	bool operator!= (BasicStringView other) {
		return !(*this == other);
	}

	size_t findFirst(Char c, size_t start_from = 0) {
		for(size_t i = start_from; i < p_length; i++)
			if(p_pointer[i] == c)
				return i;

		return size_t(-1);
	}

	size_t findLast(Char c) {
		for(size_t i = p_length; i > 0; i--)
			if(p_pointer[i - 1] == c)
				return i - 1;
		
		return size_t(-1);
	}

	BasicStringView subString(size_t from, size_t size) {
		assert(from + size <= p_length);
		return BasicStringView(p_pointer + from, size);
	}

	template<typename T>
	Optional<T> toNumber() {
		T value = 0;
		for(size_t i = 0; i < p_length; i++) {
			if(!(p_pointer[i] >= '0' && p_pointer[i] <= '9'))
				return Optional<T>();
			value = value * 10 + (p_pointer[i] - '0');
		}
		return value;
	}

private:
	const Char *p_pointer;
	size_t p_length;
};

typedef BasicStringView<char> StringView;

template<typename Char, typename Allocator>
class BasicString {
public:
	typedef Char CharType;

	friend void swap(BasicString &a, BasicString &b) {
		swap(a.p_allocator, b.p_allocator);
		swap(a.p_buffer, b.p_buffer);
		swap(a.p_length, b.p_length);
	}

	BasicString(Allocator &allocator)
	: p_allocator(&allocator), p_buffer(nullptr), p_length(0) { }

	BasicString(Allocator &allocator, const Char *c_string)
	: p_allocator(&allocator) {
		p_length = strlen(c_string);
		p_buffer = (Char *)p_allocator->allocate(sizeof(Char) * p_length);
		memcpy(p_buffer, c_string, sizeof(Char) * p_length);
	}

	BasicString(Allocator &allocator, const Char *buffer, size_t size)
	: p_allocator(&allocator), p_length(size) {
		p_buffer = (Char *)p_allocator->allocate(sizeof(Char) * p_length);
		memcpy(p_buffer, buffer, sizeof(Char) * p_length);
	}

	BasicString(Allocator &allocator, const BasicStringView<Char> &view)
	: p_allocator(&allocator), p_length(view.size()) {
		p_buffer = (Char *)p_allocator->allocate(sizeof(Char) * p_length);
		memcpy(p_buffer, view.data(), sizeof(Char) * p_length);
	}
	
	BasicString(Allocator &allocator, size_t size, Char c = 0)
	: p_allocator(&allocator), p_length(size) {
		p_buffer = (Char *)p_allocator->allocate(sizeof(Char) * p_length);
		for(size_t i = 0; i < size; i++)
			p_buffer[i] = c;
	}

	BasicString(const BasicString &other)
	: p_allocator(other.p_allocator), p_length(other.p_length) {
		p_buffer = (Char *)p_allocator->allocate(sizeof(Char) * p_length);
		memcpy(p_buffer, other.p_buffer, sizeof(Char) * p_length);
	}

	void resize(size_t new_length) {
		size_t copy_length = p_length;
		if(copy_length > new_length)
			copy_length = new_length;

		Char *new_buffer = (Char *)p_allocator->allocate(sizeof(Char) * new_length);
		memcpy(new_buffer, p_buffer, sizeof(Char) * copy_length);
		
		if(p_buffer != nullptr)
			p_allocator->free(p_buffer);
		p_length = new_length;
		p_buffer = new_buffer;
	}
	
	BasicString operator+ (const BasicStringView<Char> &other) {
		size_t new_length = p_length + other.size();
		Char *new_buffer = (Char *)p_allocator->allocate(sizeof(Char) * new_length);
		memcpy(new_buffer, p_buffer, sizeof(Char) * p_length);
		memcpy(new_buffer + p_length, other.data(), sizeof(Char) * other.size());
		
		return BasicString(*p_allocator, new_buffer, new_length);
	}

	BasicString &operator+= (const BasicStringView<Char> &other) {
		size_t new_length = p_length + other.size();
		Char *new_buffer = (Char *)p_allocator->allocate(sizeof(Char) * new_length);
		memcpy(new_buffer, p_buffer, sizeof(Char) * p_length);
		memcpy(new_buffer + p_length, other.data(), sizeof(Char) * other.size());
		
		if(p_buffer != nullptr)
			p_allocator->free(p_buffer);
		p_length = new_length;
		p_buffer = new_buffer;

		return *this;
	}

	BasicString &operator= (BasicString other) {
		swap(*this, other);
		return *this;
	}

	Char *data() {
		return p_buffer;
	}
	const Char *data() const {
		return p_buffer;
	}

	Char &operator[] (size_t index) {
		return p_buffer[index];
	}	
	const Char &operator[] (size_t index) const {
		return p_buffer[index];
	}

	size_t size() const {
		return p_length;
	}

	bool operator== (const BasicStringView<Char> &other) const {
		if(p_length != other.size())
			return false;
		for(size_t i = 0; i < p_length; i++)
			if(p_buffer[i] != other[i])
				return false;
		return true;
	}

	operator BasicStringView<Char> () const {
		return BasicStringView<Char>(p_buffer, p_length);
	}

private:
	Allocator *p_allocator;
	Char *p_buffer;
	size_t p_length;
};

template<typename T>
class DefaultHasher;

template<typename Char>
class DefaultHasher<BasicStringView<Char>> {
public:
	unsigned int operator() (const BasicStringView<Char> &string) {
		unsigned int hash = 0;
		for(size_t i = 0; i < string.size(); i++)
			hash += 31 * hash + string[i];
		return hash;
	}
};

template<typename Char, typename Allocator>
class DefaultHasher<BasicString<Char, Allocator>> {
public:
	unsigned int operator() (const BasicString<Char, Allocator> &string) {
		unsigned int hash = 0;
		for(size_t i = 0; i < string.size(); i++)
			hash += 31 * hash + string[i];
		return hash;
	}
};

template<typename Allocator>
using String = BasicString<char, Allocator>;

template<typename Allocator, typename T>
String<Allocator> uintToString(Allocator &allocator, T number, int radix = 10) {
	if(number == 0)
		return String<Allocator>(allocator, "0");
	
	int length = 0;
	T rem = number;
	while(rem > 0) {
		length++;
		rem /= radix;
	}

	const char *digits = "0123456789abcdef";
	String<Allocator> string(allocator, length);

	for(int i = length - 1; i >= 0; i--) {
		string[i] = digits[number % radix];
		number /= radix;
	}

	return move(string);
}

template<typename P, typename Allocator>
struct Print<P, String<Allocator>> {
	static void print(P &printer, String<Allocator> string) {
		for(size_t i = 0; i < string.size(); i++)
			printer.print(string.data()[i]);
	}
};

template<typename P>
struct Print<P, StringView> {
	static void print(P &printer, StringView string) {
		for(size_t i = 0; i < string.size(); i++)
			printer.print(string.data()[i]);
	}
};

} // namespace frigg

#endif // FRIGG_STRING_HPP

