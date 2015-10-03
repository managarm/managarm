
namespace frigg {
namespace protobuf {

class BufferWriter {
public:
	BufferWriter(uint8_t *buffer, size_t length)
	: p_buffer(buffer), p_index(0), p_length(length) { }

	void poke(uint8_t byte) {
		assert(p_index < p_length);
		p_buffer[p_index++] = byte;
	}
	void poke(const void *source, size_t length) {
		assert(p_index + length <= p_length);
		memcpy(p_buffer + p_index, source, length);
		p_index += length;
	}
	void advance(size_t peek_length) {
		assert(p_index + peek_length <= p_length);
		p_index += peek_length;
	}

	size_t offset() const {
		return p_index;
	}
	size_t size() const {
		return p_index;
	}
	const uint8_t *data() const {
		return p_buffer;
	}

private:
	uint8_t *p_buffer;
	size_t p_index;
	size_t p_length;
};

class BufferReader {
public:
	BufferReader(const uint8_t *buffer, size_t length)
	: p_index(0), p_length(length), p_buffer(buffer) { }
	
	uint8_t peek() {
		assert(p_index < p_length);
		return p_buffer[p_index++];
	}
	void peek(void *dest, size_t peek_length) {
		assert(p_index + peek_length <= p_length);
		memcpy(dest, &p_buffer[p_index], peek_length);
		p_index += peek_length;
	}
	void advance(size_t peek_length) {
		assert(p_index + peek_length <= p_length);
		p_index += peek_length;
	}
	
	size_t offset() const {
		return p_index;
	}
	bool atEnd() {
		return p_index == p_length;
	}

private:
	size_t p_index;
	size_t p_length;
	const uint8_t *p_buffer;
};

// --------------------------------------------------------
// Basic encoding / decoding stuff
// --------------------------------------------------------

template<typename Writer>
void pokeVarint(Writer &writer, uint64_t value) {
	do {
		uint8_t byte = value & 0x7F;
		uint64_t remainder = value >> 7;
		
		if(remainder != 0)
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

		if((byte & 0x80) == 0)
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
	kWireVarint = 0,
	kWire64 = 1,
	kWireDelimited = 2,
	kWire32 = 5,

	kWireOfInt32 = kWireVarint,
	kWireOfUInt32 = kWireVarint,
	kWireOfInt64 = kWireVarint,
	kWireOfUInt64 = kWireVarint
};

typedef uint32_t Field;

struct Header {
	Header(Field field, WireFormat wire)
	: field(field), wire(wire) { }

	Field field;
	WireFormat wire;
};

// --------------------------------------------------------
// Output functions
// --------------------------------------------------------

template<typename Writer>
void pokeHeader(Writer &writer, Header header) {
	assert(header.wire < 8);
	pokeVarint(writer, (header.field << 3) | header.wire);
}

template<typename Writer>
void emitInt32(Writer &writer, Field field, int32_t value) {
	pokeHeader(writer, Header(field, kWireVarint));
	pokeVarint(writer, value);
}

template<typename Writer>
void emitUInt32(Writer &writer, Field field, uint32_t value) {
	pokeHeader(writer, Header(field, kWireVarint));
	pokeVarint(writer, value);
}

template<typename Writer>
void emitInt64(Writer &writer, Field field, int64_t value) {
	pokeHeader(writer, Header(field, kWireVarint));
	pokeVarint(writer, value);
}

template<typename Writer>
void emitUInt64(Writer &writer, Field field, uint64_t value) {
	pokeHeader(writer, Header(field, kWireVarint));
	pokeVarint(writer, value);
}

template<typename Writer>
void emitString(Writer &writer, Field field, const char *string, size_t length) {
	pokeHeader(writer, Header(field, kWireDelimited));
	pokeVarint(writer, length);
	writer.poke(string, length);
}

// --------------------------------------------------------
// Input functions
// --------------------------------------------------------

template<typename Reader>
Header fetchHeader(Reader &reader) {
	uint32_t word = peekVarint(reader);
	return Header(word >> 3, (WireFormat)(word & 0x07));
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

} } // namespace frigg::protobuf

