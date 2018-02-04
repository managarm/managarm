#ifndef POSIX_SUBSYSTEM_SYSFS_HPP
#define POSIX_SUBSYSTEM_SYSFS_HPP

#include "vfs.hpp"

std::shared_ptr<FsLink> getSysfs();

std::shared_ptr<FsLink> sysfsMkdir(FsNode *node, std::string name);

#endif // POSIX_SUBSYSTEM_SYSFS_HPP
