
#include "file.hpp"

namespace nl_socket {

std::shared_ptr<File> createSocketFile();
std::array<std::shared_ptr<File>, 2> createSocketPair();

} // namespace nl_socket

