#include <assert.h>

#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <render-text.hpp>

namespace eir {

namespace {

constexpr int fontWidth = 8;
constexpr int fontHeight = 16;

frg::optional<EirFramebuffer> &accessGlobalFb() {
	static frg::eternal<frg::optional<EirFramebuffer>> singleton;
	return *singleton;
}

struct FbLogHandler : LogHandler {
	// Check whether eir can log to this framebuffer.
	static bool suitable(const EirFramebuffer &fb) {
		if (fb.fbBpp != 32)
			return false;
		if (fb.fbAddress + fb.fbHeight * fb.fbPitch > UINTPTR_MAX)
			return false;
		return true;
	}

	void emit(frg::string_view line) override {
		auto &fb = *accessGlobalFb();
		for (size_t i = 0; i < line.size(); ++i) {
			auto c = line[i];
			if (c == '\n') {
				outputX_ = 0;
				outputY_++;
			} else if (outputX_ >= fb.fbWidth / fontWidth) {
				outputX_ = 0;
				outputY_++;
			} else if (outputY_ >= fb.fbHeight / fontHeight) {
				// TODO: Scroll.
			} else {
				renderChars(
				    physToVirt<void>(fb.fbAddress),
				    fb.fbPitch / sizeof(uint32_t),
				    outputX_,
				    outputY_,
				    &c,
				    1,
				    15,
				    -1,
				    std::integral_constant<int, fontWidth>{},
				    std::integral_constant<int, fontHeight>{}
				);
				outputX_++;
			}
		}
		outputX_ = 0;
		outputY_++;
	}

private:
	unsigned int outputX_{0};
	unsigned int outputY_{0};
};

constinit FbLogHandler fbLogHandler;

} // anonymous namespace

void initFramebuffer(const EirFramebuffer &fb) {
	auto &globalFb = accessGlobalFb();
	// Right now, we only support a single FB.
	// If we want to support multiple ones, we may also need multiple log handlers
	// (e.g., because some may be suitable for eir logging while others may not be).
	assert(!globalFb);
	globalFb = fb;

	if (FbLogHandler::suitable(fb)) {
		enableLogHandler(&fbLogHandler);
	} else {
		infoLogger() << "eir: Framebuffer is not suitable for logging" << frg::endlog;
	}
}

const EirFramebuffer *getFramebuffer() {
	auto &globalFb = accessGlobalFb();
	if (!globalFb)
		return nullptr;
	return &(*globalFb);
}

initgraph::Stage *getFramebufferAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.framebuffer-available"};
	return &s;
}

} // namespace eir
