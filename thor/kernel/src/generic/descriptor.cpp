
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// RdDescriptor
// --------------------------------------------------------

RdDescriptor::RdDescriptor(KernelSharedPtr<RdFolder> &&folder)
		: p_folder(frigg::move(folder)) { }

KernelUnsafePtr<RdFolder> RdDescriptor::getFolder() {
	return p_folder;
}

} // namespace thor

