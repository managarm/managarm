#pragma once

#include <eir/interface.hpp>

namespace eir {

void initFramebuffer(const EirFramebuffer &fb);

// Return the known framebuffer or nullptr if there is none.
const EirFramebuffer *getFramebuffer();

} // namespace eir
