
#include "file.hpp"

std::shared_ptr<ProxyFile> createUnixSocketFile();
std::array<std::shared_ptr<ProxyFile>, 2> createUnixSocketPair();

