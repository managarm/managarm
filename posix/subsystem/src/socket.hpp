
#include "file.hpp"

std::shared_ptr<File> createUnixSocketFile();
std::array<std::shared_ptr<File>, 2> createUnixSocketPair();

