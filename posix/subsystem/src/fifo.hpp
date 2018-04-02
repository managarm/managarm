
#include "file.hpp"

namespace fifo {

std::array<smarter::shared_ptr<File, FileHandle>, 2> createPair();

} // namespace fifo

