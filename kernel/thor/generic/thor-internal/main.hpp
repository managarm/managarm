#pragma once

#include <eir/interface.hpp>
#include <initgraph.hpp>

namespace thor {

EirInfo *getEirInfo();
frg::string_view getKernelCmdline();

struct GlobalInitEngine : public initgraph::Engine {
	virtual ~GlobalInitEngine() = default;
protected:
	void onRealizeNode(initgraph::Node *node) override;
	void onRealizeEdge(initgraph::Edge *node) override;
	void preActivate(initgraph::Node *node) override;
	void postActivate(initgraph::Node *node) override;
	void reportUnreached(initgraph::Node *node) override;
	void onUnreached() override;
};

extern GlobalInitEngine globalInitEngine;
initgraph::Stage *getTaskingAvailableStage();

} // namespace thor
