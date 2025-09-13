#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>

namespace {
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

[[noreturn]] void eirGenericMain(const GenericInfo &genericInfo) {
	if (genericInfo.cmdline) {
		cmdline = genericInfo.cmdline;
	}

	if (genericInfo.hasFb) {
		initFramebuffer(genericInfo.fb);
	}

	genericDebugFlags = genericInfo.debugFlags;

	eirMain();
}

} // namespace eir
