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

struct ClientRequest {
  enum {
    kField_identifier = 1,
    kField_base_address = 2
  };
};

struct ServerResponse {
  enum {
    kField_phdr_pointer = 1,
    kField_phdr_entry_size = 2,
    kField_phdr_count = 3,
    kField_entry = 4,
    kField_dynamic = 5,
    kField_segments = 6
  };
};

} } // namespace managarm::ld_server
