#include "file.hpp"

namespace extern_socket {
async::result<smarter::shared_ptr<File, FileHandle>> createSocket(helix::BorrowedLane lane, int type, int proto);
}
