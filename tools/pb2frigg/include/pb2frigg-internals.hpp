#pragma once

#include <stdint.h>

#include <frg/macros.hpp>

namespace {
namespace pb2frigg {

class BufferWriter {
public:
	BufferWriter(uint8_t *buffer, size_t length)
	: buffer_{buffer}, index_{0}, length_{length} { }

	void poke(uint8_t byte) {
		FRG_ASSERT(index_ < length_);
		buffer_[index_++] = byte;
	}
	void poke(const void *source, size_t length) {
		FRG_ASSERT(index_ + length <= length_);
		memcpy(buffer_ + index_, source, length);
		index_ += length;
	}
	void advance(size_t peek_length) {
		FRG_ASSERT(index_ + peek_length <= length_);
		index_ += peek_length;
	}

	size_t offset() const {
		return index_;
	}
	size_t size() const {
		return index_;
	}
	const uint8_t *data() const {
		return buffer_;
	}

private:
	uint8_t *buffer_;
	size_t index_;
	size_t length_;
};

class BufferReader {
public:
	BufferReader(const uint8_t *buffer, size_t length)
	: buffer_{buffer}, index_{0}, length_{length} { }

	uint8_t peek() {
		FRG_ASSERT(index_ < length_);
		return buffer_[index_++];
	}
	void peek(void *dest, size_t peek_length) {
		FRG_ASSERT(index_ + peek_length <= length_);
		memcpy(dest, &buffer_[index_], peek_length);
		index_ += peek_length;
	}
	void advance(size_t peek_length) {
		FRG_ASSERT(index_ + peek_length <= length_);
		index_ += peek_length;
	}

	size_t offset() const {
		return index_;
	}
	bool atEnd() {
		return index_ == length_;
	}

private:
	const uint8_t *buffer_;
	size_t index_;
	size_t length_;
};

// --------------------------------------------------------
// Basic encoding / decoding stuff
// --------------------------------------------------------

template<typename Writer>
void pokeVarint(Writer &writer, uint64_t value) {
	do {
		uint8_t byte = value & 0x7F;
		uint64_t remainder = value >> 7;

		if(remainder)
			byte |= 0x80;
		writer.poke(byte);

		value = remainder;
	}while(value != 0);
}

template<typename Reader>
uint64_t peekVarint(Reader &reader) {
	uint64_t value = 0;
	int i = 0;
	while(true) {
		uint64_t byte = reader.peek();
		value |= (byte & 0x7F) << (i * 7);

		if(!(byte & 0x80))
			return value;
		i++;
	}
}

inline int varintSize(uint64_t value) {
	if(value == 0)
		return 1;
	int size = 0;
	while(value > 0) {
		size++;
		value = value >> 7;
	}
	return size;
}

inline uint64_t encodeZigZag(int64_t value) {
	if(value >= 0) {
		return (uint64_t)value << 1;
	}else{
		return ((uint64_t)-value) | 1;
	}
}

inline int64_t decodeZigZag(uint64_t encoded) {
	if((encoded & 1) != 0) {
		return -(int64_t)(encoded >> 1);
	}else{
		return encoded >> 1;
	}
}

enum WireFormat {
	wireVarint = 0,
	wire64 = 1,
	wireDelimited = 2,
	wire32 = 5,

	wireOfInt32 = wireVarint,
	wireOfUInt32 = wireVarint,
	wireOfInt64 = wireVarint,
	wireOfUInt64 = wireVarint
};

typedef uint32_t Field;

struct Header {
	Field field;
	WireFormat wire;
};

// --------------------------------------------------------
// Output functions
// --------------------------------------------------------

template<typename Writer>
void pokeHeader(Writer &writer, Header header) {
	FRG_ASSERT(header.wire < 8);
	pokeVarint(writer, (header.field << 3) | header.wire);
}

template<typename Writer>
void emitInt32(Writer &writer, Field field, int32_t value) {
	pokeHeader(writer, Header{field, wireVarint});
	pokeVarint(writer, value);
}

template<typename Writer>
void emitUInt32(Writer &writer, Field field, uint32_t value) {
	pokeHeader(writer, Header{field, wireVarint});
	pokeVarint(writer, value);
}

template<typename Writer>
void emitInt64(Writer &writer, Field field, int64_t value) {
	pokeHeader(writer, Header{field, wireVarint});
	pokeVarint(writer, value);
}

template<typename Writer>
void emitUInt64(Writer &writer, Field field, uint64_t value) {
	pokeHeader(writer, Header{field, wireVarint});
	pokeVarint(writer, value);
}

template<typename Writer>
void emitString(Writer &writer, Field field, const char *string, size_t length) {
	pokeHeader(writer, Header{field, wireDelimited});
	pokeVarint(writer, length);
	writer.poke(string, length);
}

// --------------------------------------------------------
// Input functions
// --------------------------------------------------------

template<typename Reader>
Header fetchHeader(Reader &reader) {
	uint32_t word = peekVarint(reader);
	return Header{word >> 3, (WireFormat)(word & 0x07)};
}

template<typename Reader>
int32_t fetchInt32(Reader &reader) {
	return peekVarint(reader);
}

template<typename Reader>
uint32_t fetchUInt32(Reader &reader) {
	return peekVarint(reader);
}


template<typename Reader>
int64_t fetchInt64(Reader &reader) {
	return peekVarint(reader);
}

template<typename Reader>
uint64_t fetchUInt64(Reader &reader) {
	return peekVarint(reader);
}

} } // namespace pb2frigg
