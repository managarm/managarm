
#include "file.hpp"

namespace nl_socket {

smarter::shared_ptr<File, FileHandle> createSocketFile();
std::array<smarter::shared_ptr<File, FileHandle>, 2> createSocketPair();

} // namespace nl_socket

