
namespace frigg {
namespace protobuf {

template<size_t numBytes>
class FixedWriter {
public:
	FixedWriter() : p_index(0) { }

	void poke(uint8_t byte) {
		ASSERT(p_index < numBytes);
		p_buffer[p_index++] = byte;
	}
	void poke(const void *source, size_t length) {
		ASSERT(p_index + length <= numBytes);
		memcpy(p_buffer + p_index, source, length);
		p_index += length;
	}

	size_t size() const {
		return p_index;
	}
	const uint8_t *data() const {
		return p_buffer;
	}

private:
	size_t p_index;
	uint8_t p_buffer[numBytes];
};

class BufferReader {
public:
	BufferReader(const uint8_t *buffer, size_t length)
	: p_index(0), p_length(length), p_buffer(buffer) { }
	
	uint8_t peek() {
		ASSERT(p_index < p_length);
		return p_buffer[p_index++];
	}
	void peek(void *dest, size_t peek_length) {
		ASSERT(p_index + peek_length <= p_length);
		memcpy(dest, &p_buffer[p_index], peek_length);
		p_index += peek_length;
	}
	
	void skip(size_t skip_length) {
		ASSERT(p_index + skip_length <= p_length);
		p_index += skip_length;
	}

	bool atEnd() {
		return p_index == p_length;
	}

private:
	size_t p_index;
	size_t p_length;
	const uint8_t *p_buffer;
};

template<typename Reader>
class LimitedReader {
public:
	LimitedReader(Reader &reader, size_t remaining)
	: p_remaining(remaining), p_reader(reader) { }
	
	uint8_t peek() {
		ASSERT(p_remaining > 0);
		p_remaining--;
		return p_reader.peek();
	}

	bool atEnd() {
		return p_remaining == 0;
	}
	
private:
	size_t p_remaining;
	Reader &p_reader;
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

uint64_t encodeZigZag(int64_t value) {
	if(value >= 0) {
		return (uint64_t)value << 1;
	}else{
		return ((uint64_t)-value) | 1;
	}
}

int64_t decodeZigZag(uint64_t encoded) {
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
	ASSERT(header.wire < 8);
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
void emitCString(Writer &writer, Field field, const char *string) {
	size_t length = strlen(string);
	pokeHeader(writer, Header(field, kWireDelimited));
	pokeVarint(writer, length);
	writer.poke(string, length);
}

template<typename Writer, typename Message>
void emitMessage(Writer &writer, Field field, const Message &message) {
	pokeHeader(writer, Header(field, kWireDelimited));
	pokeVarint(writer, message.size());
	writer.poke(message.data(), message.size());
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
uint32_t fetchUInt64(Reader &reader) {
	return peekVarint(reader);
}

template<typename Reader>
LimitedReader<Reader> fetchMessage(Reader &reader) {
	size_t length = peekVarint(reader);
	return LimitedReader<Reader>(reader, length);
}

template<typename Reader>
size_t fetchString(Reader &reader, char *buffer, size_t max_length) {
	size_t length = peekVarint(reader);
	ASSERT(length <= max_length);
	reader.peek(buffer, length);
	return length;
}

template<typename Reader>
void skip(Reader &reader, WireFormat wire) {
	if(wire == kWireVarint) {
		peekVarint(reader);
	}else{
		ASSERT(!"Unexpected wire format");
	}
}

} } // namespace frigg::protobuf

