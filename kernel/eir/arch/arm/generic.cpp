#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>

namespace {
EirFramebuffer genericFb;
bool hasGenericFb = false;
uint32_t genericDebugFlags = 0;
} // namespace

namespace eir {

initgraph::Task setupMiscInfo{
    &globalInitEngine,
    "aarch64.setup-misc-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] { info_ptr->debugFlags |= genericDebugFlags; }
};

initgraph::Task setupFramebufferInfo{
    &globalInitEngine,
    "aarch64.setup-framebuffer-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    if (hasGenericFb) {
		    fb = &info_ptr->frameBuffer;
		    info_ptr->frameBuffer = genericFb;
	    } else {
		    infoLogger() << "eir: Got no framebuffer!" << frg::endlog;
	    }
    }
};

[[noreturn]] void eirGenericMain(const GenericInfo &genericInfo) {
	if (genericInfo.cmdline) {
		cmdline = genericInfo.cmdline;
	}

	if (genericInfo.hasFb) {
		genericFb = genericInfo.fb;
		hasGenericFb = true;
	}

	genericDebugFlags = genericInfo.debugFlags;

	eirMain();
}

} // namespace eir
