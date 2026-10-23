#pragma once

#include <async/basic.hpp>
#include <helix/ipc.hpp>

#include "gpt.hpp"

namespace blockfs {

// TODO: generalize this to also work with regular files.
async::detached serveSwapTarget(helix::UniqueLane lane, gpt::Partition *partition);

} // namespace blockfs
