#pragma once

#include <initgraph.hpp>

namespace eir {

struct GlobalInitEngine final : initgraph::Engine {
	void preActivate(initgraph::Node *node) override;
	void onUnreached() override;
};

extern GlobalInitEngine globalInitEngine;

} // namespace eir
