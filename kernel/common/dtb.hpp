#pragma once

#include <arch/variable.hpp>
#include <assert.h>
#include <cstddef>
#include <frg/optional.hpp>
#include <frg/span.hpp>
#include <frg/string.hpp>
#include <frg/utility.hpp>
#include <string.h>
#include <type_traits>

namespace dtb {

// Points to an array of uint32_t cells (usually within a DT property).
// For example, this can be used to point to #address-cells-many cells.
struct Cells {
	Cells() = default;

	Cells(const std::byte *p, size_t numCells) : p_{p}, numCells_{numCells} {}

	size_t numCells() { return numCells_; }

	template <typename T>
	[[nodiscard]] bool read(T &v) {
		if (sizeof(T) < sizeof(uint32_t) * numCells_)
			return false; // Fail if T cannot hold enough data.

		v = 0;
		for (size_t i = 0; i < numCells_; ++i) {
			arch::scalar_storage<uint32_t, arch::big_endian> s;
			memcpy(&s, p_ + sizeof(uint32_t) * i, sizeof(uint32_t));
			v |= static_cast<T>(s.load()) << (numCells_ - i - 1) * 32;
		}
		return true;
	}

	[[nodiscard]] bool intoSlice(Cells &cells, size_t offset, size_t numCells) {
		if (offset + numCells > numCells_)
			return false; // Fail if out-of-bounds.
		cells = {p_ + sizeof(uint32_t) * offset, numCells};
		return true;
	}

	template <typename T>
	[[nodiscard]] bool readSlice(T &v, size_t offset, size_t numCells) {
		Cells cells;
		if (!intoSlice(cells, offset, numCells))
			return false;
		return cells.read(v);
	}

private:
	const std::byte *p_{nullptr};
	size_t numCells_{0};
};

// Sentinel that Accessor can be compared against.
struct EndOfProperty {};
constexpr EndOfProperty endOfProperty;

// Points to an offset within a DT property.
struct Accessor {
	Accessor(frg::span<const std::byte> data, size_t offset) : data_{data}, offset_{offset} {}

	bool operator==(EndOfProperty) {
		assert(offset_ <= data_.size());
		return offset_ == data_.size();
	}

	Accessor &operator+=(size_t numBytes) {
		assert(offset_ + numBytes <= data_.size());
		offset_ += numBytes;
		return *this;
	}

	size_t offset() { return offset_; }

	[[nodiscard]] bool intoCells(Cells &cells, size_t numCells) {
		if (offset_ + sizeof(uint32_t) * numCells > data_.size())
			return false; // Fail if out-of-bounds.
		cells = {data_.data() + offset_, numCells};
		return true;
	}

	template <typename T>
	[[nodiscard]] bool readCells(T &v, size_t numCells) {
		Cells cells;
		if (!intoCells(cells, numCells))
			return false;
		return cells.read(v);
	}

private:
	frg::span<const std::byte> data_;
	size_t offset_{0};
};

} // namespace dtb

struct DtbHeader {
	arch::scalar_storage<uint32_t, arch::big_endian> magic;
	arch::scalar_storage<uint32_t, arch::big_endian> totalsize;
	arch::scalar_storage<uint32_t, arch::big_endian> off_dt_struct;
	arch::scalar_storage<uint32_t, arch::big_endian> off_dt_strings;
	arch::scalar_storage<uint32_t, arch::big_endian> off_mem_rsvmap;
	arch::scalar_storage<uint32_t, arch::big_endian> version;
	arch::scalar_storage<uint32_t, arch::big_endian> last_comp_version;
	arch::scalar_storage<uint32_t, arch::big_endian> boot_cpuid_phys;
	arch::scalar_storage<uint32_t, arch::big_endian> size_dt_strings;
	arch::scalar_storage<uint32_t, arch::big_endian> size_dt_struct;
};

struct DeviceTreeMemoryReservation {
	uint64_t address;
	uint64_t size;

	bool operator==(const DeviceTreeMemoryReservation &other) const = default;
};

enum class Tag : uint32_t { beginNode = 1, endNode = 2, prop = 3, nop = 4, end = 9 };

struct DeviceTreeNode;

template <typename T>
concept DeviceTreeWalker = requires(T t, DeviceTreeNode n) {
	t.push(n);
	t.pop();
};

struct DeviceTree {
	DeviceTree(void *data)
	: data_{reinterpret_cast<std::byte *>(data)},
	  memoryReservations_{nullptr} {
		DtbHeader header;
		memcpy(&header, data, sizeof(header));
		assert(header.magic.load() == 0xd00dfeed);

		stringsBlock_ = data_ + header.off_dt_strings.load();
		structureBlock_ = data_ + header.off_dt_struct.load();

		totalSize_ = header.totalsize.load();

		memoryReservations_ = {data_ + header.off_mem_rsvmap.load()};
	}

	size_t size() const { return totalSize_; }

	void *data() const { return data_; }

	DeviceTreeNode rootNode();

	const std::byte *stringsBlock() const { return stringsBlock_; }

	template <DeviceTreeWalker T>
	void walkTree(T &&walker);

	struct MemoryReservationRange {
		MemoryReservationRange(std::byte *begin) : begin_{begin}, end_{nullptr} {
			if (begin) {
				Iterator it{begin};
				while (*it != DeviceTreeMemoryReservation{0, 0})
					it++;

				end_ = it.ptr_;
			}
		}

		struct Iterator {
			friend MemoryReservationRange;

			Iterator(std::byte *ptr) : ptr_{ptr} {}

			bool operator==(const Iterator &other) const = default;

			DeviceTreeMemoryReservation operator*() {
				arch::scalar_storage<uint64_t, arch::big_endian> address;
				arch::scalar_storage<uint64_t, arch::big_endian> size;

				memcpy(&address, ptr_, 8);
				memcpy(&size, ptr_ + 8, 8);

				return {address.load(), size.load()};
			}

			Iterator &operator++() {
				next();
				return *this;
			}

			Iterator &operator++(int) {
				next();
				return *this;
			}

		private:
			void next() { ptr_ += 16; }

			std::byte *ptr_;
		};

		Iterator begin() const { return begin_; }

		Iterator end() const { return end_; }

	private:
		Iterator begin_;
		Iterator end_;
	};

	MemoryReservationRange memoryReservations() const { return memoryReservations_; }

private:
	std::byte *data_;

	std::byte *stringsBlock_;
	std::byte *structureBlock_;

	uint32_t totalSize_;

	MemoryReservationRange memoryReservations_;
};

struct DeviceTreeProperty {
	DeviceTreeProperty() : name_{nullptr}, data_{nullptr, 0} {}

	DeviceTreeProperty(const char *name, frg::span<std::byte> data)
	: name_{name},
	  data_{data.data(), data.size()} {}

	DeviceTreeProperty(const char *name, frg::span<const std::byte> data)
	: name_{name},
	  data_{data} {}

	const char *name() const { return name_; }

	const void *data() const { return data_.data(); }

	size_t size() const { return data_.size(); }

	dtb::Accessor access() { return dtb::Accessor{data_, 0}; }

	uint32_t asU32(size_t offset = 0) const {
		assert(offset + 4 <= data_.size());

		arch::scalar_storage<uint32_t, arch::big_endian> v;
		memcpy(&v, data_.data() + offset, 4);

		return v.load();
	}

	uint64_t asU64(size_t offset = 0) const {
		assert(offset + 8 <= data_.size());

		arch::scalar_storage<uint64_t, arch::big_endian> v;
		memcpy(&v, data_.data() + offset, 8);

		return v.load();
	}

	frg::optional<frg::string_view> asString(size_t index = 0) const {
		size_t total = 0;
		const char *off = reinterpret_cast<const char *>(data_.data());
		for (size_t i = 0; i < index; i++) {
			total += strnlen(off + total, data_.size() - total) + 1;
			if (total >= data_.size())
				return frg::null_opt;
		}
		return frg::string_view{off + total, strnlen(off + total, data_.size() - total)};
	}

	uint64_t asPropArrayEntry(size_t nCells, size_t offset = 0) const {
		if (nCells == 0)
			return 0;
		else if (nCells == 1)
			return asU32(offset);
		else if (nCells == 2)
			return asU64(offset);

		assert(!"Invalid amount of cells");
		return -1;
	}

	DeviceTreeProperty subProperty(size_t offset) {
		return {name_, data_.subspan(offset)};
	}

private:
	const char *name_;
	frg::span<const std::byte> data_;
};

namespace detail {
inline Tag readTag(std::byte *&ptr) {
	Tag t;
	do {
		arch::scalar_storage<uint32_t, arch::big_endian> tag;
		memcpy(&tag, ptr, 4);
		ptr += 4;
		t = static_cast<Tag>(tag.load());
	} while (t == Tag::nop);

	assert(t != Tag::end);

	return t;
}

inline const char *readStringInline(std::byte *&ptr) {
	auto str = reinterpret_cast<char *>(ptr);
	ptr += (strlen(str) + 4) & ~3;

	return str;
}

inline const char *readString(DeviceTree *tree, std::byte *&ptr) {
	arch::scalar_storage<uint32_t, arch::big_endian> strOff;
	memcpy(&strOff, ptr, 4);
	ptr += 4;

	return reinterpret_cast<const char *>(tree->stringsBlock()) + strOff.load();
}

inline uint32_t readLength(std::byte *&ptr) {
	arch::scalar_storage<uint32_t, arch::big_endian> len;
	memcpy(&len, ptr, 4);
	ptr += 4;

	return len.load();
}

inline frg::span<std::byte> readPropData(std::byte *&ptr, uint32_t len) {
	auto dataPtr = ptr;
	ptr += (len + 3) & ~3;

	return {dataPtr, len};
}

inline void skipProp(std::byte *&ptr) {
	auto len = readLength(ptr);
	ptr += 4;              // skip name
	ptr += (len + 3) & ~3; // skip data
}
} // namespace detail

struct DeviceTreeNode {
	DeviceTreeNode()
	: tree_{},
	  base_{},
	  nodeOff_{},
	  propOff_{},
	  name_{},
	  properties_{nullptr, nullptr, nullptr} {}

	DeviceTreeNode(DeviceTree *tree, std::byte *base)
	: tree_{tree},
	  base_{base},
	  nodeOff_{},
	  propOff_{},
	  name_{},
	  properties_{nullptr, nullptr, nullptr} {
		std::byte *tmp = base;

		auto tag = detail::readTag(tmp);
		assert(tag == Tag::beginNode);
		name_ = detail::readStringInline(tmp);

		propOff_ = tmp;
		nodeOff_ = findNodeOff_();

		properties_ = {tree, propOff_, nodeOff_};
	}

	bool operator==(const DeviceTreeNode &other) const {
		return tree_ == other.tree_ && base_ == other.base_;
	}

	template <DeviceTreeWalker T>
	void walkChildren(T &&walker) {
		std::byte *ptr = nodeOff_;
		int depth = 0;

		while (true) {
			auto tag = detail::readTag(ptr);

			switch (tag) {
				case Tag::beginNode:
					// construct a node and push it
					depth++;
					walker.push({tree_, ptr - 4});
					(void)detail::readStringInline(ptr);
					break;

				case Tag::prop:
					// skip properties of subnodes
					// TODO: ensure nodes and properties are not interleaved?
					detail::skipProp(ptr);
					break;

				case Tag::endNode:
					// pop a node
					walker.pop();
					if (!depth--)
						return;
					break;

				default:
					assert(!"Unknown tag");
			}
		}
	}

	frg::optional<DeviceTreeProperty> findProperty(frg::string_view name) const {
		for (auto prop : properties_)
			if (name == prop.name())
				return prop;

		return frg::null_opt;
	}

	template <typename Pred, typename F>
	void discoverSubnodes(Pred pred, F onDiscover) {
		struct {
			void push(DeviceTreeNode node) {
				depth++;

				if (depth != 1)
					return;

				if (pred(node))
					onDiscover(node);
			}

			void pop() { depth--; }

			int depth;
			Pred pred;
			F onDiscover;
		} walker{0, std::move(pred), std::move(onDiscover)};

		walkChildren(walker);
	}

	const char *name() const { return name_; }

	DeviceTree *tree() const { return tree_; }

	struct PropertyRange {
		PropertyRange(DeviceTree *tree, std::byte *begin, std::byte *end)
		: begin_{tree, begin},
		  end_{tree, end} {}

		struct Iterator {
			Iterator(DeviceTree *tree, std::byte *ptr) : tree_{tree}, ptr_{ptr} {}

			bool operator==(const Iterator &other) const = default;

			DeviceTreeProperty operator*() {
				auto p = ptr_;

				auto tag = detail::readTag(p);
				assert(tag == Tag::prop);

				auto len = detail::readLength(p);
				auto name = detail::readString(tree_, p);
				auto data = detail::readPropData(p, len);
				return DeviceTreeProperty{name, data};
			}

			Iterator &operator++() {
				next();
				return *this;
			}

			Iterator &operator++(int) {
				next();
				return *this;
			}

		private:
			void next() {
				ptr_ += 4; // skip tag
				detail::skipProp(ptr_);
				detail::readTag(ptr_); // skip potential nop tags
				ptr_ -= 4;             // rewind back to tag
			}

			DeviceTree *tree_;
			std::byte *ptr_;
		};

		Iterator begin() const { return begin_; }

		Iterator end() const { return end_; }

	private:
		Iterator begin_;
		Iterator end_;
	};

	PropertyRange properties() const { return properties_; }

private:
	std::byte *findNodeOff_() {
		std::byte *ptr = propOff_;

		Tag tag;
		while (true) {
			tag = detail::readTag(ptr);

			if (tag != Tag::prop)
				break;

			detail::skipProp(ptr);
		}

		return ptr - 4; // pointer to tag
	}

	DeviceTree *tree_;
	std::byte *base_;

	std::byte *nodeOff_;
	std::byte *propOff_;
	const char *name_;

	PropertyRange properties_;
};

inline DeviceTreeNode DeviceTree::rootNode() { return {this, structureBlock_}; }

template <DeviceTreeWalker T>
inline void DeviceTree::walkTree(T &&walker) {
	auto rn = rootNode();

	walker.push(rn);
	rn.walkChildren(static_cast<T &>(walker));
	walker.pop();
}
