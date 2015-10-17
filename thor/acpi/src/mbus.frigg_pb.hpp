// This file is auto-generated from mbus.proto
// Do not try to edit it manually!

namespace managarm {
namespace mbus {

struct CntReqType {
  enum {
    REGISTER = 1,
    QUERY_IF = 2
  };
};

struct SvrReqType {
  enum {
    BROADCAST = 1,
    REQUIRE_IF = 2
  };
};

template<typename Allocator>
class Capability {
public:
  typedef frigg::String<Allocator> String;

  template<typename T>
  using Vector = frigg::Vector<T, Allocator>;

  Capability(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_name(allocator) { }

  inline const String &name() const {
    return m_name;
  }
  inline void set_name(const String &value) {
    m_name = value;
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    size_t name_length = m_name.size();
    p_cachedSize += frigg::protobuf::varintSize(name_length);
    p_cachedSize += name_length;
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitString(writer, 1, m_name.data(), m_name.size());
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
        size_t name_length = peekVarint(reader);
        m_name.resize(name_length);
        reader.peek(m_name.data(), name_length);
      } break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  String m_name;
};

template<typename Allocator>
class Interface {
public:
  typedef frigg::String<Allocator> String;

  template<typename T>
  using Vector = frigg::Vector<T, Allocator>;

  Interface(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_name(allocator) { }

  inline const String &name() const {
    return m_name;
  }
  inline void set_name(const String &value) {
    m_name = value;
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    size_t name_length = m_name.size();
    p_cachedSize += frigg::protobuf::varintSize(name_length);
    p_cachedSize += name_length;
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitString(writer, 1, m_name.data(), m_name.size());
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
        size_t name_length = peekVarint(reader);
        m_name.resize(name_length);
        reader.peek(m_name.data(), name_length);
      } break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  String m_name;
};

template<typename Allocator>
class CntRequest {
public:
  typedef frigg::String<Allocator> String;

  template<typename T>
  using Vector = frigg::Vector<T, Allocator>;

  CntRequest(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_req_type(0),
    m_caps(allocator),
    m_ifs(allocator),
    m_object_id(0) { }

  inline int64_t req_type() const {
    return m_req_type;
  }
  inline void set_req_type(int64_t value) {
    m_req_type = value;
  }

  inline void add_caps(const ::managarm::mbus::Capability<Allocator> &message) {
    m_caps.push(message);
  }
  inline size_t caps_size() const {
    return m_caps.size();
  }
  inline const ::managarm::mbus::Capability<Allocator> &caps(size_t i) const {
    return m_caps[i];
  }

  inline void add_ifs(const ::managarm::mbus::Interface<Allocator> &message) {
    m_ifs.push(message);
  }
  inline size_t ifs_size() const {
    return m_ifs.size();
  }
  inline const ::managarm::mbus::Interface<Allocator> &ifs(size_t i) const {
    return m_ifs[i];
  }

  inline int64_t object_id() const {
    return m_object_id;
  }
  inline void set_object_id(int64_t value) {
    m_object_id = value;
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_req_type);
    p_cachedSize += m_caps.size() * frigg::protobuf::varintSize(2 << 3);
    for(size_t i = 0; i < m_caps.size(); i++) {
      size_t caps_length = m_caps[i].ByteSize();
      p_cachedSize += frigg::protobuf::varintSize(caps_length);
      p_cachedSize += caps_length;
    }
    p_cachedSize += m_ifs.size() * frigg::protobuf::varintSize(3 << 3);
    for(size_t i = 0; i < m_ifs.size(); i++) {
      size_t ifs_length = m_ifs[i].ByteSize();
      p_cachedSize += frigg::protobuf::varintSize(ifs_length);
      p_cachedSize += ifs_length;
    }
    p_cachedSize += frigg::protobuf::varintSize(4 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_object_id);
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitInt64(writer, 1, m_req_type);
    for(size_t i = 0; i < m_caps.size(); i++) {
      pokeHeader(writer, frigg::protobuf::Header(2, frigg::protobuf::kWireDelimited));
      pokeVarint(writer, m_caps[i].GetCachedSize());
      m_caps[i].SerializeWithCachedSizesToArray((uint8_t *)array + writer.offset(), m_caps[i].GetCachedSize());
      writer.advance(m_caps[i].GetCachedSize());
    }
    for(size_t i = 0; i < m_ifs.size(); i++) {
      pokeHeader(writer, frigg::protobuf::Header(3, frigg::protobuf::kWireDelimited));
      pokeVarint(writer, m_ifs[i].GetCachedSize());
      m_ifs[i].SerializeWithCachedSizesToArray((uint8_t *)array + writer.offset(), m_ifs[i].GetCachedSize());
      writer.advance(m_ifs[i].GetCachedSize());
    }
    frigg::protobuf::emitInt64(writer, 4, m_object_id);
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
        m_req_type = fetchInt64(reader);
        break;
      case 2: {
        assert(header.wire == frigg::protobuf::kWireDelimited);
        size_t caps_length = peekVarint(reader);
        ::managarm::mbus::Capability<Allocator> element(*p_allocator);
        element.ParseFromArray((uint8_t *)array + reader.offset(), caps_length);
        m_caps.push(frigg::move(element));
        reader.advance(caps_length);
      } break;
      case 3: {
        assert(header.wire == frigg::protobuf::kWireDelimited);
        size_t ifs_length = peekVarint(reader);
        ::managarm::mbus::Interface<Allocator> element(*p_allocator);
        element.ParseFromArray((uint8_t *)array + reader.offset(), ifs_length);
        m_ifs.push(frigg::move(element));
        reader.advance(ifs_length);
      } break;
      case 4:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_object_id = fetchInt64(reader);
        break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  int64_t m_req_type;
  Vector<::managarm::mbus::Capability<Allocator>> m_caps;
  Vector<::managarm::mbus::Interface<Allocator>> m_ifs;
  int64_t m_object_id;
};

template<typename Allocator>
class SvrResponse {
public:
  typedef frigg::String<Allocator> String;

  template<typename T>
  using Vector = frigg::Vector<T, Allocator>;

  SvrResponse(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_object_id(0) { }

  inline int64_t object_id() const {
    return m_object_id;
  }
  inline void set_object_id(int64_t value) {
    m_object_id = value;
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_object_id);
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitInt64(writer, 1, m_object_id);
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
        m_object_id = fetchInt64(reader);
        break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  int64_t m_object_id;
};

template<typename Allocator>
class SvrRequest {
public:
  typedef frigg::String<Allocator> String;

  template<typename T>
  using Vector = frigg::Vector<T, Allocator>;

  SvrRequest(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0),
    m_req_type(0),
    m_object_id(0),
    m_caps(allocator),
    m_ifs(allocator) { }

  inline int64_t req_type() const {
    return m_req_type;
  }
  inline void set_req_type(int64_t value) {
    m_req_type = value;
  }

  inline int64_t object_id() const {
    return m_object_id;
  }
  inline void set_object_id(int64_t value) {
    m_object_id = value;
  }

  inline void add_caps(const ::managarm::mbus::Capability<Allocator> &message) {
    m_caps.push(message);
  }
  inline size_t caps_size() const {
    return m_caps.size();
  }
  inline const ::managarm::mbus::Capability<Allocator> &caps(size_t i) const {
    return m_caps[i];
  }

  inline void add_ifs(const ::managarm::mbus::Interface<Allocator> &message) {
    m_ifs.push(message);
  }
  inline size_t ifs_size() const {
    return m_ifs.size();
  }
  inline const ::managarm::mbus::Interface<Allocator> &ifs(size_t i) const {
    return m_ifs[i];
  }

  size_t ByteSize() {
    p_cachedSize = 0;
    p_cachedSize += frigg::protobuf::varintSize(1 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_req_type);
    p_cachedSize += frigg::protobuf::varintSize(2 << 3);
    p_cachedSize += frigg::protobuf::varintSize(m_object_id);
    p_cachedSize += m_caps.size() * frigg::protobuf::varintSize(3 << 3);
    for(size_t i = 0; i < m_caps.size(); i++) {
      size_t caps_length = m_caps[i].ByteSize();
      p_cachedSize += frigg::protobuf::varintSize(caps_length);
      p_cachedSize += caps_length;
    }
    p_cachedSize += m_ifs.size() * frigg::protobuf::varintSize(4 << 3);
    for(size_t i = 0; i < m_ifs.size(); i++) {
      size_t ifs_length = m_ifs[i].ByteSize();
      p_cachedSize += frigg::protobuf::varintSize(ifs_length);
      p_cachedSize += ifs_length;
    }
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
    frigg::protobuf::emitInt64(writer, 1, m_req_type);
    frigg::protobuf::emitInt64(writer, 2, m_object_id);
    for(size_t i = 0; i < m_caps.size(); i++) {
      pokeHeader(writer, frigg::protobuf::Header(3, frigg::protobuf::kWireDelimited));
      pokeVarint(writer, m_caps[i].GetCachedSize());
      m_caps[i].SerializeWithCachedSizesToArray((uint8_t *)array + writer.offset(), m_caps[i].GetCachedSize());
      writer.advance(m_caps[i].GetCachedSize());
    }
    for(size_t i = 0; i < m_ifs.size(); i++) {
      pokeHeader(writer, frigg::protobuf::Header(4, frigg::protobuf::kWireDelimited));
      pokeVarint(writer, m_ifs[i].GetCachedSize());
      m_ifs[i].SerializeWithCachedSizesToArray((uint8_t *)array + writer.offset(), m_ifs[i].GetCachedSize());
      writer.advance(m_ifs[i].GetCachedSize());
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
        m_req_type = fetchInt64(reader);
        break;
      case 2:
        assert(header.wire == frigg::protobuf::kWireVarint);
        m_object_id = fetchInt64(reader);
        break;
      case 3: {
        assert(header.wire == frigg::protobuf::kWireDelimited);
        size_t caps_length = peekVarint(reader);
        ::managarm::mbus::Capability<Allocator> element(*p_allocator);
        element.ParseFromArray((uint8_t *)array + reader.offset(), caps_length);
        m_caps.push(frigg::move(element));
        reader.advance(caps_length);
      } break;
      case 4: {
        assert(header.wire == frigg::protobuf::kWireDelimited);
        size_t ifs_length = peekVarint(reader);
        ::managarm::mbus::Interface<Allocator> element(*p_allocator);
        element.ParseFromArray((uint8_t *)array + reader.offset(), ifs_length);
        m_ifs.push(frigg::move(element));
        reader.advance(ifs_length);
      } break;
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
  int64_t m_req_type;
  int64_t m_object_id;
  Vector<::managarm::mbus::Capability<Allocator>> m_caps;
  Vector<::managarm::mbus::Interface<Allocator>> m_ifs;
};

template<typename Allocator>
class CntResponse {
public:
  typedef frigg::String<Allocator> String;

  template<typename T>
  using Vector = frigg::Vector<T, Allocator>;

  CntResponse(Allocator &allocator)
  : p_allocator(&allocator), p_cachedSize(0) { }

  size_t ByteSize() {
    p_cachedSize = 0;
    return p_cachedSize;
  }
  size_t GetCachedSize() {
    return p_cachedSize;
  }

  void SerializeWithCachedSizesToArray(void *array, size_t length) {
    frigg::protobuf::BufferWriter writer((uint8_t *)array, length);
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
      default:
        assert(!"Unexpected field number");
      }
    }
  }

private:
  Allocator *p_allocator;
  size_t p_cachedSize;
};

} } // namespace managarm::mbus
