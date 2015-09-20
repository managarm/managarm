// This file is auto-generated from posix.proto
// Do not try to edit it manually!

namespace managarm {
namespace posix {

struct Errors {
  enum {
    SUCCESS = 0,
    FILE_NOT_FOUND = 1,
    ACCESS_DENIED = 2,
    ALREADY_EXISTS = 3,
    ILLEGAL_REQUEST = 4,
    NO_SUCH_FD = 5
  };
};

struct ClientRequestType {
  enum {
    SPAWN = 1,
    OPEN = 2,
    READ = 3,
    WRITE = 4,
    CLOSE = 5
  };
};

template<typename Allocator>
class ClientRequest {
public:
  typedef frigg::util::String<Allocator> String;

  template<typename T>
  using Vector = frigg::util::Vector<T, Allocator>;

  ClientRequest(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_request_type(0),
    m_path(allocator),
    m_flags(0),
    m_fd(0),
    m_size(0),
    m_buffer(allocator) { }

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

  inline int32_t flags() const {
    return m_flags;
  }
  inline void set_flags(int32_t value) {
    m_flags = value;
  }

  inline int32_t fd() const {
    return m_fd;
  }
  inline void set_fd(int32_t value) {
    m_fd = value;
  }

  inline int32_t size() const {
    return m_size;
  }
  inline void set_size(int32_t value) {
    m_size = value;
  }

  inline const String &buffer() const {
    return m_buffer;
  }
  inline void set_buffer(const String &value) {
    m_buffer = value;
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_request_type);
    p_cachedSize += frigg::protobuf::varintSize(2 << 3);
    size_t path_length = m_path.size();
    p_cachedSize += frigg::protobuf::varintSize(path_length);
    p_cachedSize += path_length;
    p_cachedSize += frigg::protobuf::varintSize(3 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_flags);
    p_cachedSize += frigg::protobuf::varintSize(4 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_fd);
    p_cachedSize += frigg::protobuf::varintSize(5 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_size);
    p_cachedSize += frigg::protobuf::varintSize(6 << 3);
    size_t buffer_length = m_buffer.size();
    p_cachedSize += frigg::protobuf::varintSize(buffer_length);
    p_cachedSize += buffer_length;
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitInt64(writer, 1, m_request_type);
    frigg::protobuf::emitString(writer, 2, m_path.data(), m_path.size());
    frigg::protobuf::emitInt32(writer, 3, m_flags);
    frigg::protobuf::emitInt32(writer, 4, m_fd);
    frigg::protobuf::emitInt32(writer, 5, m_size);
    frigg::protobuf::emitString(writer, 6, m_buffer.data(), m_buffer.size());
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
      case 3:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_flags = fetchInt32(reader);
        break;
      case 4:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_fd = fetchInt32(reader);
        break;
      case 5:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_size = fetchInt32(reader);
        break;
      case 6: {
        assert(header.wire == frigg::protobuf::kWireDelimited);
        size_t buffer_length = peekVarint(reader);
        m_buffer.resize(buffer_length);
        reader.peek(m_buffer.data(), buffer_length);
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
  int32_t m_flags;
  int32_t m_fd;
  int32_t m_size;
  String m_buffer;
};

template<typename Allocator>
class ServerResponse {
public:
  typedef frigg::util::String<Allocator> String;

  template<typename T>
  using Vector = frigg::util::Vector<T, Allocator>;

  ServerResponse(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_error(0),
    m_fd(0),
    m_buffer(allocator) { }

  inline int32_t error() const {
    return m_error;
  }
  inline void set_error(int32_t value) {
    m_error = value;
  }

  inline int32_t fd() const {
    return m_fd;
  }
  inline void set_fd(int32_t value) {
    m_fd = value;
  }

  inline const String &buffer() const {
    return m_buffer;
  }
  inline void set_buffer(const String &value) {
    m_buffer = value;
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(3 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_error);
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_fd);
    p_cachedSize += frigg::protobuf::varintSize(2 << 3);
    size_t buffer_length = m_buffer.size();
    p_cachedSize += frigg::protobuf::varintSize(buffer_length);
    p_cachedSize += buffer_length;
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitInt32(writer, 3, m_error);
    frigg::protobuf::emitInt32(writer, 1, m_fd);
    frigg::protobuf::emitString(writer, 2, m_buffer.data(), m_buffer.size());
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
      case 3:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_error = fetchInt32(reader);
        break;
      case 1:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_fd = fetchInt32(reader);
        break;
      case 2: {
        assert(header.wire == frigg::protobuf::kWireDelimited);
        size_t buffer_length = peekVarint(reader);
        m_buffer.resize(buffer_length);
        reader.peek(m_buffer.data(), buffer_length);
      } break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  int32_t m_error;
  int32_t m_fd;
  String m_buffer;
};

} } // namespace managarm::posix
