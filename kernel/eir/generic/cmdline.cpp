#include <assert.h>

#include <eir-internal/cmdline.hpp>
#include <eir-internal/debug.hpp>
#include <frg/small_vector.hpp>

namespace eir {

namespace {

// Command line chunks can come from:
// * Boot protocol (UEFI, limine, etc.)
// * UKIs
// * Device tree
// * PXE data
// Expand this when more sources are added.
constexpr size_t maxChunks = 4;

auto &getCmdlineChunks() {
	static frg::eternal<frg::static_vector<frg::string_view, maxChunks>> singleton;
	return *singleton;
}

} // namespace

void extendCmdline(frg::string_view chunk) {
	auto &cmdlineChunks = getCmdlineChunks();
	if (cmdlineChunks.size() == maxChunks) {
		infoLogger() << "eir: Too many command line chunks. Ignoring: " << chunk << frg::endlog;
		return;
	}
	assert(cmdlineChunks.size() < maxChunks);
	cmdlineChunks.push_back(chunk);
}

std::span<frg::string_view> getCmdline() { return getCmdlineChunks(); }

} // namespace eir
