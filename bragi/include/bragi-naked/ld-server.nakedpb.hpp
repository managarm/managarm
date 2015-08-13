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

struct Segment {
  enum {
    kField_virt_address = 1,
    kField_virt_length = 2,
    kField_access = 3
  };
};

struct Object {
  enum {
    kField_entry = 1,
    kField_segments = 2
  };
};

} } // namespace managarm::ld_server
