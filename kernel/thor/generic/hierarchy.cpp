#include <frg/manual_box.hpp>

#include <thor-internal/debug.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/main.hpp>

namespace thor {

Hierarchy::Hierarchy(CtorToken, smarter::shared_ptr<Hierarchy> parent, frg::string<KernelAlloc> tag)
: parent_{std::move(parent)}, tag_{std::move(tag)} { }

std::expected<smarter::shared_ptr<Hierarchy>, Error> Hierarchy::createRoot() {
	auto ptr = smarter::allocate_shared<Hierarchy>(
		*kernelAlloc,
		CtorToken{},
		smarter::shared_ptr<Hierarchy>{},
		frg::string<KernelAlloc>{*kernelAlloc, "root"}
	);
	if(!ptr)
		return std::unexpected{Error::noMemory};
	return ptr;
}

std::expected<smarter::shared_ptr<Hierarchy>, Error> Hierarchy::extend(
	smarter::shared_ptr<Hierarchy> parent, frg::string<KernelAlloc> tag
) {
	if(!parent)
		return std::unexpected{Error::illegalArgs};
	auto ptr = smarter::allocate_shared<Hierarchy>(
		*kernelAlloc, CtorToken{}, std::move(parent), std::move(tag)
	);
	if(!ptr)
		return std::unexpected{Error::noMemory};
	return ptr;
}

namespace {
	constinit frg::manual_box<smarter::shared_ptr<Hierarchy>> rootHierarchy_;

	initgraph::Task initRootHierarchy{&globalInitEngine, "generic.init-root-hierarchy",
		initgraph::Entails{getTaskingAvailableStage()},
		[] {
			auto created = Hierarchy::createRoot();
			if(!created)
				panicLogger() << "thor: Failed to allocate root hierarchy"
						<< frg::endlog;
			rootHierarchy_.initialize(std::move(*created));
		}
	};
}

smarter::shared_ptr<Hierarchy> rootHierarchy() {
	return *rootHierarchy_;
}

} // namespace thor
