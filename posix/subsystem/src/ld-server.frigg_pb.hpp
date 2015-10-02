// This file is auto-generated from ld-server.proto
// Do not try to edit it manually!

namespace managarm {
namespace ld_server {

struct Access {
  enum {
    READ_ONLY = 1,
    READ_WRITE = 2,
    READ_EXECUTE = 3
  };
};

template<typename Allocator>
class Segment {
public:
  typedef frigg::String<Allocator> String;

  template<typename T>
  using Vector = frigg::Vector<T, Allocator>;

  Segment(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_virt_address(0),
    m_virt_length(0),
    m_access(0) { }

  inline uint64_t virt_address() const {
    return m_virt_address;
  }
  inline void set_virt_address(uint64_t value) {
    m_virt_address = value;
  }

  inline uint64_t virt_length() const {
    return m_virt_length;
  }
  inline void set_virt_length(uint64_t value) {
    m_virt_length = value;
  }

  inline int64_t access() const {
    return m_access;
  }
  inline void set_access(int64_t value) {
    m_access = value;
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_virt_address);
    p_cachedSize += frigg::protobuf::varintSize(2 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_virt_length);
    p_cachedSize += frigg::protobuf::varintSize(3 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_access);
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitUInt64(writer, 1, m_virt_address);
    frigg::protobuf::emitUInt64(writer, 2, m_virt_length);
    frigg::protobuf::emitInt64(writer, 3, m_access);
    assert(writer.offset() == length);
  }
  void SerializeToString(String *string) {
    string->resize(ByteSize());
    SerializeWithCachedSizesToArray(string->data(), string->size());
  }

  void ParseFromArray(const void *buffer, size_t buffer_size) {
    const uint8_t *array = static_cast<const uint8_t *>(buffer);
    frigg::protobuf::BufferReader reader(array, buffer_size);
    while(!reader.atEnd()) {
      auto header = fetchHeader(reader);
      switch(header.field) {
      case 1:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_virt_address = fetchUInt64(reader);
        break;
      case 2:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_virt_length = fetchUInt64(reader);
        break;
      case 3:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_access = fetchInt64(reader);
        break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  uint64_t m_virt_address;
  uint64_t m_virt_length;
  int64_t m_access;
};

template<typename Allocator>
class ClientRequest {
public:
  typedef frigg::String<Allocator> String;

  template<typename T>
  using Vector = frigg::Vector<T, Allocator>;

  ClientRequest(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_identifier(allocator),
    m_base_address(0) { }

  inline const String &identifier() const {
    return m_identifier;
  }
  inline void set_identifier(const String &value) {
    m_identifier = value;
  }

  inline uint64_t base_address() const {
    return m_base_address;
  }
  inline void set_base_address(uint64_t value) {
    m_base_address = value;
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    size_t identifier_length = m_identifier.size();
    p_cachedSize += frigg::protobuf::varintSize(identifier_length);
    p_cachedSize += identifier_length;
    p_cachedSize += frigg::protobuf::varintSize(2 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_base_address);
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitString(writer, 1, m_identifier.data(), m_identifier.size());
    frigg::protobuf::emitUInt64(writer, 2, m_base_address);
    assert(writer.offset() == length);
  }
  void SerializeToString(String *string) {
    string->resize(ByteSize());
    SerializeWithCachedSizesToArray(string->data(), string->size());
  }

  void ParseFromArray(const void *buffer, size_t buffer_size) {
    const uint8_t *array = static_cast<const uint8_t *>(buffer);
    frigg::protobuf::BufferReader reader(array, buffer_size);
    while(!reader.atEnd()) {
      auto header = fetchHeader(reader);
      switch(header.field) {
      case 1: {
        assert(header.wire == frigg::protobuf::kWireDelimited);
        size_t identifier_length = peekVarint(reader);
        m_identifier.resize(identifier_length);
        reader.peek(m_identifier.data(), identifier_length);
      } break;
      case 2:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_base_address = fetchUInt64(reader);
        break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  String m_identifier;
  uint64_t m_base_address;
};

template<typename Allocator>
class ServerResponse {
public:
  typedef frigg::String<Allocator> String;

  template<typename T>
  using Vector = frigg::Vector<T, Allocator>;

  ServerResponse(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_phdr_pointer(0),
    m_phdr_entry_size(0),
    m_phdr_count(0),
    m_entry(0),
    m_dynamic(0),
    m_segments(allocator) { }

  inline uint64_t phdr_pointer() const {
    return m_phdr_pointer;
  }
  inline void set_phdr_pointer(uint64_t value) {
    m_phdr_pointer = value;
  }

  inline uint64_t phdr_entry_size() const {
    return m_phdr_entry_size;
  }
  inline void set_phdr_entry_size(uint64_t value) {
    m_phdr_entry_size = value;
  }

  inline uint64_t phdr_count() const {
    return m_phdr_count;
  }
  inline void set_phdr_count(uint64_t value) {
    m_phdr_count = value;
  }

  inline uint64_t entry() const {
    return m_entry;
  }
  inline void set_entry(uint64_t value) {
    m_entry = value;
  }

  inline uint64_t dynamic() const {
    return m_dynamic;
  }
  inline void set_dynamic(uint64_t value) {
    m_dynamic = value;
  }

  inline void add_segments(const ::managarm::ld_server::Segment<Allocator> &message) {
    m_segments.push(message);
  }
  inline size_t segments_size() const {
    return m_segments.size();
  }
  inline const ::managarm::ld_server::Segment<Allocator> &segments(size_t i) const {
    return m_segments[i];
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_phdr_pointer);
    p_cachedSize += frigg::protobuf::varintSize(2 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_phdr_entry_size);
    p_cachedSize += frigg::protobuf::varintSize(3 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_phdr_count);
    p_cachedSize += frigg::protobuf::varintSize(4 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_entry);
    p_cachedSize += frigg::protobuf::varintSize(5 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_dynamic);
    p_cachedSize += m_segments.size() * frigg::protobuf::varintSize(6 << 3);
    for(size_t i = 0; i < m_segments.size(); i++) {
      size_t segments_length = m_segments[i].ByteSize();
      p_cachedSize += frigg::protobuf::varintSize(segments_length);
      p_cachedSize += segments_length;
    }
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitUInt64(writer, 1, m_phdr_pointer);
    frigg::protobuf::emitUInt64(writer, 2, m_phdr_entry_size);
    frigg::protobuf::emitUInt64(writer, 3, m_phdr_count);
    frigg::protobuf::emitUInt64(writer, 4, m_entry);
    frigg::protobuf::emitUInt64(writer, 5, m_dynamic);
    for(size_t i = 0; i < m_segments.size(); i++) {
      pokeHeader(writer, frigg::protobuf::Header(6, frigg::protobuf::kWireDelimited));
      pokeVarint(writer, m_segments[i].GetCachedSize());
      m_segments[i].SerializeWithCachedSizesToArray((uint8_t *)array + writer.offset(), m_segments[i].GetCachedSize());
      writer.advance(m_segments[i].GetCachedSize());
    }
    assert(writer.offset() == length);
  }
  void SerializeToString(String *string) {
    string->resize(ByteSize());
    SerializeWithCachedSizesToArray(string->data(), string->size());
  }

  void ParseFromArray(const void *buffer, size_t buffer_size) {
    const uint8_t *array = static_cast<const uint8_t *>(buffer);
    frigg::protobuf::BufferReader reader(array, buffer_size);
    while(!reader.atEnd()) {
      auto header = fetchHeader(reader);
      switch(header.field) {
      case 1:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_phdr_pointer = fetchUInt64(reader);
        break;
      case 2:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_phdr_entry_size = fetchUInt64(reader);
        break;
      case 3:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_phdr_count = fetchUInt64(reader);
        break;
      case 4:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_entry = fetchUInt64(reader);
        break;
      case 5:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_dynamic = fetchUInt64(reader);
        break;
      case 6: {
        assert(header.wire == frigg::protobuf::kWireDelimited);
        size_t segments_length = peekVarint(reader);
        ::managarm::ld_server::Segment<Allocator> element(*p_allocator);
        element.ParseFromArray((uint8_t *)array + reader.offset(), segments_length);
        m_segments.push(frigg::traits::move(element));
        reader.advance(segments_length);
      } break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  uint64_t m_phdr_pointer;
  uint64_t m_phdr_entry_size;
  uint64_t m_phdr_count;
  uint64_t m_entry;
  uint64_t m_dynamic;
  Vector<::managarm::ld_server::Segment<Allocator>> m_segments;
};

} } // namespace managarm::ld_server
