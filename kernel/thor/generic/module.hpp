#ifndef THOR_GENERIC_MODULE_HPP
#define THOR_GENERIC_MODULE_HPP

#include <frg/string.hpp>
#include <frigg/vector.hpp>
#include "kernel_heap.hpp"
#include "address-space.hpp"

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
		frg::string<KernelAlloc> name;
		MfsNode *node;
	};

	MfsDirectory()
	: MfsNode{MfsType::directory}, _entries{*kernelAlloc} { }

	void link(frg::string<KernelAlloc> name, MfsNode *node) {
		assert(!getTarget(name));
		_entries.push(Link{frigg::move(name), node});
	}

	size_t numEntries() {
		return _entries.size();
	}

	Link getEntry(size_t i) {
		return _entries[i];
	}

	MfsNode *getTarget(frg::string_view name) {
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
	MfsRegular(frigg::SharedPtr<Memory> memory, size_t size)
	: MfsNode{MfsType::regular}, _memory{frigg::move(memory)}, _size{size} {
		assert(_size <= _memory->getLength());
	}

	frigg::SharedPtr<Memory> getMemory() {
		return _memory;
	}

	size_t size() {
		return _size;
	}

private:
	frigg::SharedPtr<Memory> _memory;
	size_t _size;
};

extern MfsDirectory *mfsRoot;

MfsNode *resolveModule(frg::string_view path);

} // namespace thor

#endif // THOR_GENERIC_MODULE_HPP
