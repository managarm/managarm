
#include "file.hpp"

namespace nl_socket {

smarter::shared_ptr<File> createSocketFile();
std::array<smarter::shared_ptr<File>, 2> createSocketPair();

} // namespace nl_socket

