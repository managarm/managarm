#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <frg/manual_box.hpp>
#include <eir/interface.hpp>
#include <frg/hash.hpp>
#include <dtb.hpp>

namespace thor {

static inline constexpr bool logNodeInfo = false;

initgraph::Stage *getDeviceTreeParsedStage() {
	static initgraph::Stage s{&globalInitEngine, "dtb.tree-parsed"};
	return &s;
}

namespace {
	frg::manual_box<DeviceTree> globalDt;

	frg::manual_box<
		frg::hash_map<
			uint32_t,
			DeviceTreeNode *,
			frg::hash<uint32_t>,
			KernelAlloc
		>
	> phandles;

	DeviceTreeNode *treeRoot;

	auto parseStringList(const ::DeviceTreeProperty &prop) {
		frg::vector<frg::string_view, KernelAlloc> list{*kernelAlloc};

		size_t i = 0;
		while (i < prop.size()) {
			frg::string_view sv{reinterpret_cast<const char *>(prop.data()) + i};
			i += sv.size() + 1;
			list.push_back(sv);
		}

		return list;
	}
} // namespace anonymous

void DeviceTreeNode::initializeWith(::DeviceTreeNode dtNode) {
	name_ = dtNode.name();
	generatePath_();

	if (auto p = dtNode.findProperty("phandle"); p) {
		phandle_ = p->asU32();
	} else if (auto p = dtNode.findProperty("linux,phandle"); p) {
		infoLogger() << "thor: warning: node \"" << name() << "\" uses legacy \"linux,phandle\" property!" << frg::endlog;
		phandle_ = p->asU32();
	}

	if (phandle_)
		phandles->insert(phandle_, this);

	for (auto prop : dtNode.properties()) {
		frg::string_view pn{prop.name()};

		if (pn == "model") {
			model_ = reinterpret_cast<const char *>(prop.data());
		} else if (pn == "compatible") {
			compatible_ = parseStringList(prop);
		} else if (pn == "#address-cells") {
			addressCells_ = prop.asU32();
			hasAddressCells_ = true;
		} else if (pn == "#size-cells") {
			sizeCells_ = prop.asU32();
			hasSizeCells_ = true;
		} else if (pn == "#interrupt-cells") {
			interruptCells_ = prop.asU32();
			hasInterruptCells_ = true;
		} else if (pn == "#iommu-cells") {
			iommuCells_ = prop.asU32();
			hasIommuCells_ = true;
		} else if (pn == "interrupt-parent") {
			interruptParentId_ = prop.asU32();
		} else if (pn == "interrupt-controller") {
			interruptController_ = true;
		} else if (pn == "reg") {
			auto addrCells = parent_->addressCells_;
			auto sizeCells = parent_->sizeCells_;

			size_t j = 0;
			while (j < prop.size()) {
				RegRange reg{};

				if (addrCells) {
					if (addrCells == 3) {
						reg.addrHi = prop.asPropArrayEntry(1, j);
						reg.addrHiValid = true;
						reg.addr = prop.asPropArrayEntry(addrCells - 1, j + 1);
						j += addrCells * 4;
					} else {
						if(j + addrCells * 4 > prop.size()) {
							infoLogger() << "thor: warning: node \"" << name() << "\": reg field isn't conforming to #addr-cells" << frg::endlog;
							reg.addr = prop.asPropArrayEntry((j + addrCells * 4 - prop.size()) / 4);
							reg_.push_back(reg);
							break;
						}
						reg.addr = prop.asPropArrayEntry(addrCells, j);
						j += addrCells * 4;
					}
				}

				if (sizeCells) {
					if(j + sizeCells * 4 > prop.size()) {
						infoLogger() << "thor: warning: node \"" << name() << "\": reg field isn't conforming to #size-cells" << frg::endlog;
						reg.size = prop.asPropArrayEntry((j + sizeCells * 4 - prop.size()) / 4);
						reg_.push_back(reg);
						break;
					}
					reg.size = prop.asPropArrayEntry(sizeCells, j);
					j += sizeCells * 4;
				}

				reg_.push_back(reg);
			}
		} else if (pn == "bus-range") {
			busRange_.from = prop.asPropArrayEntry(1, 0);
			busRange_.to = prop.asPropArrayEntry(1, 4);
		}
	}

	// Iterate again to parse things that depend on previously parsed properties
	for (auto prop : dtNode.properties()) {
		frg::string_view pn{prop.name()};

		if (pn == "ranges") {
			auto parentAddrCells = parent_->addressCells_;
			auto childAddrCells = addressCells_;
			auto sizeCells = sizeCells_;

			size_t j = 0;
			while (j < prop.size()) {
				AddrTranslateRange reg{};
				// PCI(e) buses have a 3 cell long child addresses
				if (childAddrCells == 3) {
					reg.childAddrHi = prop.asPropArrayEntry(1, j);
					j += 4;
					reg.childAddr = prop.asPropArrayEntry(2, j);
					j += 8;
					reg.childAddrHiValid = true;
				} else {
					assert(childAddrCells < 3);
					reg.childAddr = prop.asPropArrayEntry(childAddrCells, j);
					j += childAddrCells * 4;
				}

				assert(parentAddrCells < 3);
				reg.parentAddr = prop.asPropArrayEntry(parentAddrCells, j);
				j += parentAddrCells * 4;

				reg.size = prop.asPropArrayEntry(sizeCells, j);
				j += sizeCells * 4;

				ranges_.push_back(reg);
			}
		}
	}

	{
		// inherit interrupt parent from parent if we don't have one
		auto parent = parent_;
		while (!interruptParentId_) {
			if (!parent)
				break;
			if (parent->isInterruptController()) {
				assert(parent->phandle_);
				interruptParentId_ = parent->phandle_;
				continue;
			}

			interruptParentId_ = parent->interruptParentId_;
			parent = parent->parent_;
		}
	}
}

void DeviceTreeNode::finalizeInit() {
	if (interruptParentId_ != 0) {
		auto ipIt = phandles->find(interruptParentId_);
		if (ipIt == phandles->end()) {
			panicLogger() << "thor: node \"" << name() << "\" has an interrupt parent id "
				<< interruptParentId_ << " but no such node exists" << frg::endlog;
		} else {
			interruptParent_ = ipIt->get<1>();
		}
	}

	if (auto prop = dtNode_.findProperty("iommus")) {
		size_t j = 0;
		while (j < prop->size()) {
			uint32_t handle = prop->asPropArrayEntry(1, j);
			j += 4;

			auto iommuIt = phandles->find(handle);
			if (iommuIt == phandles->end()) {
				panicLogger() << "thor: node \"" << name() << "\" has an iommus property referencing id "
					<< handle << " but no such node exists" << frg::endlog;
			}

			auto iommuNode = iommuIt->get<1>();
			if (!iommuNode->isIommu()) {
				panicLogger() << "thor: node \"" << iommuNode->name() << "\" is not an iommu" << frg::endlog;
			}

			IommuReference ref{
				.node = iommuNode,
				.prop{prop->subProperty(j)}
			};
			referencedIommus_.push(ref);

			j += iommuNode->iommuCells() * 4;
		}
	}

	// perform address translation
	if (parent_ && parent_->ranges_.size()) {
		for (auto &r : reg_) {
			r.addr = parent_->translateAddress(r.addr);
		}

		for (auto &r : ranges_) {
			r.parentAddr = parent_->translateAddress(r.parentAddr);
		}
	}

	if (logNodeInfo &&
			(reg_.size()
			 || ranges_.size())) {
		infoLogger() << "Node \"" << path() << "\" has the following:" << frg::endlog;

		if (compatible_.size()) {
			infoLogger() << "\t- compatible names:" << frg::endlog;
			for (auto c : compatible_) {
				infoLogger() << "\t\t- " << c << frg::endlog;
			}
		}

		if (reg_.size()) {
			infoLogger() << "\t- resources:" << frg::endlog;
			for (auto reg : reg_) {
				if (reg.size)
					infoLogger() << "\t\t- " << (void *)reg.addr << " - "
						<< (void *)reg.size << " bytes" << frg::endlog;
				else
					infoLogger() << "\t\t- " << (void *)reg.addr << frg::endlog;
			}
		}

		if (ranges_.size()) {
			infoLogger() << "\t- ranges:" << frg::endlog;
			for (auto range : ranges_) {
				if (range.childAddrHiValid && isCompatible(dtPciCompatible)) {
					bool pref = range.childAddrHi & (1 << 30);
					uint8_t type = (range.childAddrHi >> 24) & 0b11;

					constexpr const char *typeNames[] = {"config", "I/O", "32-bit memory", "64-bit memory"};

					infoLogger() << "\t\t- child (" << (pref ? "" : "non-")
						<< "prefetchable, " << typeNames[type] << ") "
						<< (void *)range.childAddr << " translates to host "
						<< (void *)range.parentAddr << " - "
						<< (void *)range.size << " bytes" << frg::endlog;
				} else {
					infoLogger() << "\t\t- child " << (void *)range.childAddr << " translates to host "
						<< (void *)range.parentAddr << " - "
						<< (void *)range.size << " bytes" << frg::endlog;
				}
			}
		}
	}

	// Recurse into children
	for (auto &[_, child] : children_)
		child->finalizeInit();
}

void DeviceTreeNode::generatePath_() {
	frg::vector<frg::string_view, KernelAlloc> components{*kernelAlloc};

	auto p = this;
	while (p) {
		components.push_back(p->name());
		p = p->parent();
	}

	for (int i = components.size() - 1; i >= 0; i--) {
		if (components[i].size())
			path_ += "/";
		path_ += components[i];
	}
}

uint64_t DeviceTreeNode::translateAddress(uint64_t addr) const {
	// We only handle simple bus address translation
	if (!isCompatible<1>({"simple-bus"}))
		return addr;

	// This node has no translation table
	if (!ranges_.size())
		return addr;

	for (auto tr : ranges_)
		if (addr >= tr.childAddr && addr <= (tr.childAddr + tr.size))
			return tr.parentAddr + (addr - tr.childAddr);

	panicLogger() << "thor: address " << (void *)addr
		<< " doesn't fall into any of \""
		<< path() << "\"'s memory ranges" << frg::endlog;
	__builtin_unreachable();
}

DeviceTreeNode *getDeviceTreeNodeByPhandle(uint32_t phandle) {
	auto it = phandles->find(phandle);
	if (it == phandles->end())
		return nullptr;
	return it->get<1>();
}

DeviceTreeNode *getDeviceTreeNodeByPath(frg::string_view path) {
	auto remaining = path.sub_string(1, path.size() - 1);
	auto p = treeRoot;

	// remove null terminator
	if (!remaining[remaining.size() - 1])
		remaining = remaining.sub_string(0, remaining.size() - 1);

	while (remaining.size()) {
		auto nextSlash = remaining.find_first('/');
		if (nextSlash == size_t(-1))
			nextSlash = remaining.size();
		auto component = remaining.sub_string(0, nextSlash);

		auto it = p->children().find(component);

		if (it == p->children().end())
			return nullptr;
		else
			p = it->get<1>();

		if (nextSlash == remaining.size())
			break;
		remaining = remaining.sub_string(nextSlash + 1, remaining.size() - nextSlash - 1);
	}

	return p;
}

DeviceTreeNode *getDeviceTreeRoot() {
	return treeRoot;
}

static initgraph::Task initTablesTask{&globalInitEngine, "dtb.parse-dtb",
	initgraph::Entails{getDeviceTreeParsedStage()},
	[] {
		size_t dtbPageOff = getEirInfo()->dtbPtr & (kPageSize - 1);
		size_t dtbSize = (getEirInfo()->dtbSize + dtbPageOff + kPageSize - 1) & ~(kPageSize - 1);

		auto ptr = KernelVirtualMemory::global().allocate(dtbSize);
		uintptr_t va = reinterpret_cast<uintptr_t>(ptr);
		uintptr_t pa = getEirInfo()->dtbPtr & ~(kPageSize - 1);

		for (size_t i = 0; i < dtbSize; i += kPageSize) {
			KernelPageSpace::global().mapSingle4k(va, pa,
				page_access::write, CachingMode::null);
			va += kPageSize;
			pa += kPageSize;
		}

		ptr = reinterpret_cast<void *>(
				reinterpret_cast<uintptr_t>(ptr) + dtbPageOff);

		globalDt.initialize(ptr);
		phandles.initialize(frg::hash<uint32_t>{}, *kernelAlloc);

		treeRoot = frg::construct<DeviceTreeNode>(*kernelAlloc, globalDt->rootNode(), nullptr);
		treeRoot->initializeWith(globalDt->rootNode());

		infoLogger() << "thor: Booting on \"" << treeRoot->model() << "\"" << frg::endlog;

		struct {
			DeviceTreeNode *curr;

			void push(::DeviceTreeNode node) {
				auto n = frg::construct<DeviceTreeNode>(*kernelAlloc, node, curr);

				n->initializeWith(node);
				curr->attachChild(node.name(), n);
				curr = n;
			}

			void pop() {
				curr = curr->parent();
			}
		} walker{treeRoot};

		globalDt->rootNode().walkChildren(walker);

		// Initialize interruptParent etc
		// This can't be done above because the interrupt parent may not
		// have been discovered yet
		treeRoot->finalizeInit();
	}
};

} // namespace thor
