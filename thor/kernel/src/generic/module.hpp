#ifndef THOR_GENERIC_MODULE_HPP
#define THOR_GENERIC_MODULE_HPP

#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include "kernel_heap.hpp"
#include "usermem.hpp"

namespace thor {

enum class MfsType {
	null,
	directory,
	regular
};

struct MfsNode {
	MfsNode(MfsType type)
	: type{type} { }

	const MfsType type;
};

struct MfsDirectory : MfsNode {
	struct Link {
		frigg::String<KernelAlloc> name;
		MfsNode *node;
	};

	MfsDirectory()
	: MfsNode{MfsType::directory}, _entries{*kernelAlloc} { }

	void link(frigg::String<KernelAlloc> name, MfsNode *node) {
		assert(!getTarget(name));
		_entries.push(Link{frigg::move(name), node});
	}

	size_t numEntries() {
		return _entries.size();
	}

	Link getEntry(size_t i) {
		return _entries[i];
	}

	MfsNode *getTarget(frigg::StringView name) {
		for(size_t i = 0; i < _entries.size(); i++) {
			if(_entries[i].name == name)
				return _entries[i].node;
		}
		return nullptr;
	}

private:
	frigg::Vector<Link, KernelAlloc> _entries;
};

struct MfsRegular : MfsNode {
	MfsRegular(frigg::SharedPtr<Memory> memory)
	: MfsNode{MfsType::regular}, _memory{frigg::move(memory)} { }

	frigg::SharedPtr<Memory> getMemory() {
		return _memory;
	}

private:
	frigg::SharedPtr<Memory> _memory;
};

extern MfsDirectory *mfsRoot;

MfsNode *resolveModule(frigg::StringView path);

} // namespace thor

#endif // THOR_GENERIC_MODULE_HPP
