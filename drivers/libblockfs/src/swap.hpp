#pragma once

#include <async/result.hpp>
#include <protocols/fs/common.hpp>

#include "gpt.hpp"

namespace blockfs {

// Announces a swap target for a Managarm swap partition.
// TODO: generalize this to also work with regular files.
async::result<protocols::fs::Error> swapon(gpt::Partition *partition);

} // namespace blockfs
