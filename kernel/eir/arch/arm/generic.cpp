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

initgraph::Task setupInitrdInfo{
    &globalInitEngine,
    "aarch64.setup-initrd-info",
    initgraph::Requires{getInfoStructAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    auto initrd_module = bootAlloc<EirModule>(1);
	    initrd_module->physicalBase = virtToPhys(initrd);
	    initrd_module->length = initrd_image.size();
	    const char *initrd_mod_name = "initrd.cpio";
	    size_t name_length = strlen(initrd_mod_name);
	    char *name_ptr = bootAlloc<char>(name_length);
	    memcpy(name_ptr, initrd_mod_name, name_length);
	    initrd_module->namePtr = mapBootstrapData(name_ptr);
	    initrd_module->nameLength = name_length;

	    info_ptr->moduleInfo = mapBootstrapData(initrd_module);
    }
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
