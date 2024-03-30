
#include "file.hpp"

namespace eventfd {

smarter::shared_ptr<File, FileHandle> createFile(unsigned int initval, bool nonBlock, bool semaphore);

}
