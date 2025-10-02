#pragma once

namespace eir {

enum class [[nodiscard]] Error {
	none,
	// Broken DT bindings (but DTB format is fine).
	brokenBindings,
	// Address cannot be translated.
	deviceInaccessible,
	// Error that does not fall into any other category.
	other,
};

} // namespace eir
