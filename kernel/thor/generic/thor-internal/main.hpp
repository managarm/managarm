#pragma once

#include <thor-internal/initgraph.hpp>

namespace thor {

extern initgraph::Engine globalInitEngine;
initgraph::Stage *getTaskingAvailableStage();

} // namespace thor
