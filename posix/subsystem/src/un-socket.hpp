
#include "file.hpp"

namespace un_socket {

std::shared_ptr<File> createSocketFile();
std::array<std::shared_ptr<File>, 2> createSocketPair();

} // namespace un_socket

