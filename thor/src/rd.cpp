
#include "../../frigg/include/types.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"

namespace thor {

// --------------------------------------------------------
// RdFolder::Entry
// --------------------------------------------------------

RdFolder::Entry::Entry(Type type) : type(type) { }

// --------------------------------------------------------
// RdFolder
// --------------------------------------------------------

RdFolder::RdFolder()
: p_entries(*kernelAlloc) { }

void RdFolder::mount(const char *name, size_t name_length,
		SharedPtr<RdFolder, KernelAlloc> &&mounted) {
	Entry entry(kTypeMounted);
	entry.mounted = util::move(mounted);

	ASSERT(name_length <= kNameLength);
	for(size_t i = 0; i < name_length; i++)
		entry.name[i] = name[i];
	entry.nameLength = name_length;

	p_entries.push(util::move(entry));
}

void RdFolder::publish(const char *name, size_t name_length,
		AnyDescriptor &&descriptor) {
	Entry entry(kTypeDescriptor);
	entry.descriptor = util::move(descriptor);

	ASSERT(name_length <= kNameLength);
	for(size_t i = 0; i < name_length; i++)
		entry.name[i] = name[i];
	entry.nameLength = name_length;

	p_entries.push(util::move(entry));
}

bool strNEquals(const char *str1, const char *str2, size_t length) {
	for(size_t i = 0; i < length; i++) {
		if(*str1 == 0 || *str2 == 0)
			break;
		if(*str1++ != *str2++)
			return false;
	}
	return true;
}

RdFolder::Entry *RdFolder::getEntry(const char *name,
		size_t name_length) {
	for(size_t i = 0; i < p_entries.size(); i++) {
		if(p_entries[i].nameLength != name_length)
			continue;
		if(strNEquals(p_entries[i].name, name, name_length))
			return &p_entries[i];
	}
	return nullptr;
}

} // namespace thor

