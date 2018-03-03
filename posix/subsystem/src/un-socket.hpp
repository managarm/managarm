
#include "file.hpp"

namespace un_socket {

smarter::shared_ptr<File> createSocketFile();
std::array<smarter::shared_ptr<File>, 2> createSocketPair();

} // namespace un_socket

