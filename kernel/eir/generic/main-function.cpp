#include <initgraph.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/main.hpp>

namespace eir {

void GlobalInitEngine::preActivate(initgraph::Node *node) {
	infoLogger() << "eir: Running " << node->displayName() << frg::endlog;
}

void GlobalInitEngine::onUnreached() {
	infoLogger() << "eir: initgraph has cycles" << frg::endlog;

	while(1) { }
}

constinit GlobalInitEngine globalInitEngine;

initgraph::Stage *getMemoryDiscoveredStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.memory-discovered"};
	return &s;
}

initgraph::Stage *getMemorySetUpStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.memory-set-up"};
	return &s;
}

initgraph::Stage *getKernelAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.kernel-available"};
	return &s;
}

initgraph::Stage *getBootInfoBuildableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.boot-info-buildable"};
	return &s;
}

struct GlobalCtorTest {
	GlobalCtorTest() {
		infoLogger() << "Hello world from global ctor" << frg::endlog;
	}
};

GlobalCtorTest globalCtorTest;

extern "C" void eirMain() {
	infoLogger() << "Hello world from generic eirMain()" << frg::endlog;

	globalInitEngine.run();

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;
	eir::enterKernel();
}

} // namespace eir
