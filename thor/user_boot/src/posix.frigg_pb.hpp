// This file is auto-generated from posix.proto
// Do not try to edit it manually!

namespace managarm {
namespace posix {

template<typename Allocator>
class ClientRequest {
public:
  typedef frigg::util::String<Allocator> String;

  template<typename T>
  using Vector = frigg::util::Vector<T, Allocator>;

  struct RequestType {
    enum {
      SPAWN = 1
    };
  };

  ClientRequest(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_request_type(0),
    m_path(allocator) { }

  inline int64_t request_type() const {
    return m_request_type;
  }
  inline void set_request_type(int64_t value) {
    m_request_type = value;
  }

  inline const String &path() const {
    return m_path;
  }
  inline void set_path(const String &value) {
    m_path = value;
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_request_type);
    p_cachedSize += frigg::protobuf::varintSize(2 << 3);
    size_t path_length = m_path.size();
    p_cachedSize += frigg::protobuf::varintSize(path_length);
    p_cachedSize += path_length;
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitInt64(writer, 1, m_request_type);
    frigg::protobuf::emitString(writer, 2, m_path.data(), m_path.size());
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
        m_request_type = fetchInt64(reader);
        break;
      case 2: {
        assert(header.wire == frigg::protobuf::kWireDelimited);
        size_t path_length = peekVarint(reader);
        m_path.resize(path_length);
        reader.peek(m_path.data(), path_length);
      } break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  int64_t m_request_type;
  String m_path;
};

} } // namespace managarm::posix
