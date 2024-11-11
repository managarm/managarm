#pragma once

#include <initgraph.hpp>

namespace eir {

struct GlobalInitEngine final : initgraph::Engine {
	void preActivate(initgraph::Node *node) override;
	void onUnreached() override;
};

extern GlobalInitEngine globalInitEngine;

extern "C" void eirMain();

} // namespace eir
