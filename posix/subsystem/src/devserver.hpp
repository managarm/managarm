#pragma once

#include <async/result.hpp>
#include <memory>

#include "fs.hpp"

namespace devserver {

// Switch between posix-subsystem's device model and posix-devserver.
// TODO: Remove this together with the old posix-subsystem infrastructure.
inline constexpr bool useDevserver = true;

// Enumerates posix-devserver via mbus.
async::result<void> enumerate();

// Returns the sysfs root directory that posix-devserver provides over extern_fs.
async::result<smarter::shared_ptr<FsLink, LinkRc>> getSysfsRoot();

} // namespace devserver
