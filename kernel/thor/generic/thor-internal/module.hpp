#pragma once

#include <frg/string.hpp>
#include <frigg/vector.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/kernel_heap.hpp>

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
	MfsRegular(frigg::SharedPtr<MemoryView> memory, size_t size)
	: MfsNode{MfsType::regular}, _memory{frigg::move(memory)}, _size{size} {
		assert(_size <= _memory->getLength());
	}

	frigg::SharedPtr<MemoryView> getMemory() {
		return _memory;
	}

	size_t size() {
		return _size;
	}

private:
	frigg::SharedPtr<MemoryView> _memory;
	size_t _size;
};

extern MfsDirectory *mfsRoot;

MfsNode *resolveModule(frg::string_view path);

} // namespace thor
