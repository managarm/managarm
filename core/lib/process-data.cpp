#include <core/process-data.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>

namespace core {

HelHandle getProcessHierarchy() {
	static HelHandle handle = [] {
		posix::ManagarmProcessData pd;
		HEL_CHECK(helSyscall2(
			kHelCallSuper + posix::superGetProcessData,
			reinterpret_cast<HelWord>(&pd),
			sizeof(posix::ManagarmProcessData)
		));
		return pd.hierarchyHandle;
	}();
	return handle;
}

} // namespace core
