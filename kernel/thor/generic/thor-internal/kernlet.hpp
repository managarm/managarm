#pragma once

#include <frg/variant.hpp>
#include <frg/vector.hpp>
#include <thor-internal/event.hpp>

namespace thor {

struct BoundKernlet;

enum class KernletParameterType { null, offset, memoryView, bitsetEvent };

struct KernletParameterDefn {
	KernletParameterType type;
	size_t offset;
};

struct KernletObject {
	// This is only required so that BoundKernlet can access the _entry.
	// TODO: Add a getIrqAutomationEntry() function instead.
	friend struct BoundKernlet;

	KernletObject(void *entry, const frg::vector<KernletParameterType, KernelAlloc> &bind_types);

	size_t instanceSize();
	size_t numberOfBindParameters();
	const KernletParameterDefn &defnOfBindParameter(size_t index);

  private:
	void *_entry;
	frg::vector<KernletParameterDefn, KernelAlloc> _bindDefns;
	size_t _instanceSize;
};

struct BoundKernlet {
	BoundKernlet(smarter::shared_ptr<KernletObject> object);

	KernletObject *object() { return _object.get(); }

	const void *instanceStruct() { return _instance; }

	void setupOffsetBinding(size_t index, uint32_t offset);
	void setupMemoryViewBinding(size_t index, void *p);
	void setupBitsetEventBinding(size_t index, smarter::shared_ptr<BitsetEvent> event);

	int invokeIrqAutomation();

  private:
	smarter::shared_ptr<KernletObject> _object;
	char *_instance;
};

void initializeKernletCtl();

} // namespace thor
