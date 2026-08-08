#include <assert.h>

#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/log-ring.hpp>
#include <frg/utility.hpp>
#include <render-text.hpp>
#include <string.h>

namespace eir {

namespace {

constexpr int fontWidth = 8;
constexpr int fontHeight = 16;

frg::optional<EirFramebuffer> &accessGlobalFb() {
	static frg::eternal<frg::optional<EirFramebuffer>> singleton;
	return *singleton;
}

// Renders the boot log ring to the framebuffer, scrolling by dropping the oldest lines.
struct FbLogHandler : LogHandler {
	// Check whether eir can log to this framebuffer.
	static bool suitable(const EirFramebuffer &fb) {
		if (fb.fbBpp != 32)
			return false;
		if (fb.fbAddress + fb.fbHeight * fb.fbPitch > UINTPTR_MAX)
			return false;
		return true;
	}

	void initialize(const EirFramebuffer &fb) {
		window_ = physToVirt<void>(fb.fbAddress);
		pitch_ = fb.fbPitch / sizeof(uint32_t);
		numCols_ = fb.fbWidth / fontWidth;
		numRows_ = fb.fbHeight / fontHeight;

		// Paint the lines that were logged before the framebuffer became available.
		redraw();
	}

	void emit(frg::string_view line) override {
		// OutputSink::print() posted the line to the log ring already.
		if (outputY_ + rowsFor(line.size()) > numRows_) {
			redraw();
			return;
		}
		renderLine(outputY_, line);
		outputY_ += rowsFor(line.size());
	}

private:
	// Drops records until the tail of the log ring fits onto the screen, then renders it.
	void redraw() {
		auto &ring = bootLogRing();
		char buffer[maxLogLine];

		if (topPtr_ < ring.tailPtr())
			topPtr_ = ring.tailPtr();
		auto rows = rowsFrom(topPtr_);
		while (rows > numRows_) {
			auto record = ring.dequeueAt(topPtr_, buffer, maxLogLine);
			assert(record);
			rows -= rowsFor(record->size);
			topPtr_ = record->nextPtr;
		}

		outputY_ = 0;
		auto ptr = topPtr_;
		while (auto record = ring.dequeueAt(ptr, buffer, maxLogLine)) {
			renderLine(outputY_, {buffer, record->size});
			outputY_ += rowsFor(record->size);
			ptr = record->nextPtr;
		}

		for (auto y = outputY_; y < numRows_; ++y)
			blank(0, y, numCols_);
	}

	// Number of rows that the records in [ptr, head) occupy.
	unsigned int rowsFrom(uint64_t ptr) {
		auto &ring = bootLogRing();
		char buffer[maxLogLine];

		unsigned int rows = 0;
		while (auto record = ring.dequeueAt(ptr, buffer, maxLogLine)) {
			rows += rowsFor(record->size);
			ptr = record->nextPtr;
		}
		return rows;
	}

	// Number of rows that a line of `length` characters occupies once it is wrapped.
	unsigned int rowsFor(size_t length) const {
		if (!length)
			return 1;
		return (length + numCols_ - 1) / numCols_;
	}

	// Renders `line` at row `y`, wrapping and padding each row to the right edge.
	void renderLine(unsigned int y, frg::string_view line) {
		unsigned int offset = 0;
		unsigned int row = 0;
		do {
			if (y + row >= numRows_)
				return;
			auto n = frg::min(static_cast<unsigned int>(line.size()) - offset, numCols_);
			renderChars(
			    window_,
			    pitch_,
			    0,
			    y + row,
			    line.data() + offset,
			    n,
			    15,
			    -1,
			    std::integral_constant<int, fontWidth>{},
			    std::integral_constant<int, fontHeight>{}
			);
			blank(n, y + row, numCols_ - n);
			offset += n;
			++row;
		} while (offset < line.size());
	}

	void blank(unsigned int x, unsigned int y, unsigned int count) {
		if (y >= numRows_)
			return;

		char spaces[16];
		memset(spaces, ' ', sizeof(spaces));
		while (count) {
			auto n = frg::min(count, static_cast<unsigned int>(sizeof(spaces)));
			renderChars(
			    window_,
			    pitch_,
			    x,
			    y,
			    spaces,
			    n,
			    15,
			    -1,
			    std::integral_constant<int, fontWidth>{},
			    std::integral_constant<int, fontHeight>{}
			);
			x += n;
			count -= n;
		}
	}

	void *window_{nullptr};
	unsigned int pitch_{0};
	unsigned int numCols_{0};
	unsigned int numRows_{0};

	// Ring pointer of the first record that is displayed on screen.
	uint64_t topPtr_{0};
	// Row that the next line is rendered at.
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
		fbLogHandler.initialize(fb);
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
