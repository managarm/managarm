
#include "file.hpp"

namespace un_socket {

smarter::shared_ptr<File, FileHandle> createSocketFile(bool nonBlock, int32_t socktype);
std::array<smarter::shared_ptr<File, FileHandle>, 2> createSocketPair(Process *process, bool nonBlock, int32_t socktype);

} // namespace un_socket

