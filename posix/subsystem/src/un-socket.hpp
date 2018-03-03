
#include "file.hpp"

namespace un_socket {

smarter::shared_ptr<File, FileHandle> createSocketFile();
std::array<smarter::shared_ptr<File, FileHandle>, 2> createSocketPair();

} // namespace un_socket

