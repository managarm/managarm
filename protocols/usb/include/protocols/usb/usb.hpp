#pragma once

#include <arch/bits.hpp>
#include <array>
#include <assert.h>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

namespace protocols::usb {

namespace setup_type {
	enum : uint8_t {
		// The first 5 bits specify the target of the request.
		targetMask = 0x1F,
		targetDevice = 0x00,
		targetInterface = 0x01,
		targetEndpoint = 0x02,
		targetOther = 0x03,

		// The next 2 bits determine the document that specifies the request.
		specificationMask = 0x60,
		byStandard = 0x00,
		byClass = 0x20,
		byVendor = 0x40,

		// The last bit specifies the transfer direction.
		directionMask = 0x80,
		toDevice = 0x00,
		toHost = 0x80
	};
}

// Alignment makes sure that a packet doesnt cross a page boundary
struct alignas(8) SetupPacket {
	uint8_t type = 0;
	uint8_t request = 0;
	uint16_t value = 0;
	uint16_t index = 0;
	uint16_t length = 0;
};
static_assert(sizeof(SetupPacket) == 8, "Bad SetupPacket size");

namespace request_type {
	enum : uint8_t {
		getStatus = 0x00,
		clearFeature = 0x01,
		setFeature = 0x03,
		setAddress = 0x05,
		getDescriptor = 0x06,
		setDescriptor = 0x07,
		getConfig = 0x08,
		setConfig = 0x09,
		setInterface = 0x0B,

		// TODO: Move non-standard features to some other location.
		getReport = 0x01
	};
}

namespace features {
	enum : uint8_t {
		endpointHalt = 0x00,
		deviceRemoteWakeup = 0x01,
		testMode = 0x02,
	};
}

namespace descriptor_type {
	enum : uint16_t {
		device = 0x01,
		configuration = 0x02,
		string = 0x03,
		interface = 0x04,
		endpoint = 0x05,

		// TODO: Put non-standard descriptors somewhere else.
		hid = 0x21,
		report = 0x22,
		cs_interface = 0x24,
		cs_endpoint = 0x25,
	};
}

namespace usb_class {
	enum : uint8_t {
		per_interface = 0x00,
		cdc = 0x02,
		hid = 0x03,
		mass_storage = 0x08,
		cdc_data = 0x0A,
		vendor_specific = 0xFF,
	};
} // namespace usb_class

namespace cdc_subclass {
	enum : uint8_t {
		reserved = 0x00,
		ethernet = 0x06,
		ncm = 0x0D,
		mbim = 0x0E,
		vendor_specific = 0xFF,
	};
} // namespace cdc_subclass

struct DescriptorBase {
	uint8_t length;
	uint8_t descriptorType;

	template<typename T>
	static T from_vec(std::string &vec) {
		T c;
		assert(vec.size() >= sizeof(c));
		std::memcpy(&c, vec.data(), sizeof(c));
		return c;
	}
};

struct [[gnu::packed]] StringDescriptor : public DescriptorBase {
	char16_t data[0];
};

struct DeviceDescriptor : public DescriptorBase {
	uint16_t bcdUsb;
	uint8_t deviceClass;
	uint8_t deviceSubclass;
	uint8_t deviceProtocol;
	uint8_t maxPacketSize;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t manufacturer;
	uint8_t product;
	uint8_t serialNumber;
	uint8_t numConfigs;
};
//FIXME: remove alignas
//static_assert(sizeof(DeviceDescriptor) == 18, "Bad DeviceDescriptor size");

struct [[ gnu::packed ]] ConfigDescriptor : public DescriptorBase {
	uint16_t totalLength;
	uint8_t numInterfaces;
	uint8_t configValue;
	uint8_t iConfig;
	uint8_t bmAttributes;
	uint8_t maxPower;
};

struct InterfaceDescriptor : public DescriptorBase {
	uint8_t interfaceNumber;
	uint8_t alternateSetting;
	uint8_t numEndpoints;
	uint8_t interfaceClass;
	uint8_t interfaceSubClass;
	uint8_t interfaceProtocol;
	uint8_t iInterface;
};

/* CDC 1.1 5.2.3.1 */
struct [[gnu::packed]] CdcDescriptor : public DescriptorBase {
	enum class CdcSubType : uint8_t {
		Header = 0x00,
		CallManagement = 0x01,
		AbstractControl = 0x02,
		Union = 0x06,
		EthernetNetworking = 0x0F,
		Ncm = 0x1A,
		Mbim = 0x1B,
		MbimExtended = 0x1C,
	};

	CdcSubType subtype;
};

/* CDC 1.1 5.2.3.1 */
struct [[gnu::packed]] CdcHeader : public CdcDescriptor {
	uint16_t bcdCDC;
};

/* CDC 1.1 6.3 */
struct [[gnu::packed]] CdcNotificationHeader {
	enum class Notification : uint8_t {
		NETWORK_CONNECTION = 0x00,
		RESPONSE_AVAILABLE = 0x01,
		AUX_JACK_HOOK_STATE = 0x08,
		RING_DETECT = 0x09,
		SERIAL_STATE = 0x20,
		CALL_STATE_CHANGE = 0x28,
		LINE_STATE_CHANGE = 0x29,
		CONNECTION_SPEED_CHANGE = 0x2A,
	};

	uint8_t bmRequestType;
	Notification bNotificationCode;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};

/* CDC 1.1 6.3.8 */
struct [[gnu::packed]] CdcConnectionSpeedChange {
	uint32_t DlBitRate;
	uint32_t UlBitRate;
};

/* CDC 1.1 5.2.3.2 */
struct [[gnu::packed]] CdcCallManagement : public CdcDescriptor {
	uint8_t bmCapabilities;
	uint8_t bDataInterface;
};

/* CDC 1.1 5.2.3.3 */
struct [[gnu::packed]] CdcAbstractControl : public CdcDescriptor {
	uint8_t bmCapabilities;
};

/* CDC 1.1 5.2.3.8 */
struct [[gnu::packed]] CdcUnion : public CdcDescriptor {
	uint8_t bControlInterface;
	uint8_t bSubordinateInterface[1];
};

/* CDC 1.1 5.2.3.16 */
struct [[gnu::packed]] CdcEthernetNetworking : public CdcDescriptor {
	uint8_t iMACAddress;
	uint32_t bmEthernetStatistics;
	uint16_t wMaxSegmentSize;
	uint16_t wNumberMCFilters;
	uint8_t bNumberPowerFilters;
};

/* NCM 1.0 5.2.1 */
struct [[gnu::packed]] CdcNcm : public CdcDescriptor {
	uint16_t bcdNcmVersion;
	arch::bit_value<uint8_t> bmNetworkCapabilities;
};

/* MBIM 1.0 6.4 */
struct [[gnu::packed]] CdcMbim : public CdcDescriptor {
	uint16_t bcdMBIMVersion;
	uint16_t wMaxControlMessage;
	uint8_t bNumberFilters;
	uint8_t bMaxFilterSize;
	uint16_t wMaxSegmentSize;
	uint8_t bmNetworkCapabilities;
};

/* MBIM 1.0 6.5 */
struct [[gnu::packed]] CdcMbimExtended : public CdcDescriptor {
	uint16_t bcdMBIMExtendedVersion;
	uint8_t bMaxOutstandingCommandMessages;
	uint16_t wMTU;
};

struct [[ gnu::packed ]] EndpointDescriptor : public DescriptorBase {
	uint8_t endpointAddress;
	uint8_t attributes;
	uint16_t maxPacketSize;
	uint8_t interval;
};

enum class EndpointType {
	control = 0,
	isochronous,
	bulk,
	interrupt
};

// One descriptor within a configuration buffer: its header plus the bytes
// covering the whole descriptor (header included).
using DescriptorEntry = std::pair<DescriptorBase, std::span<const std::byte>>;

// Forward view over the descriptors of a single configuration buffer.
// The current entry is cached so the iterator's reference type is a true
// lvalue reference, which keeps it a std::forward_iterator (required by the
// std::views adaptors layered on top).
class ConfigurationView : public std::ranges::view_interface<ConfigurationView> {
public:
	class iterator {
	public:
		using value_type = DescriptorEntry;
		using difference_type = std::ptrdiff_t;
		using iterator_concept = std::forward_iterator_tag;

		iterator() = default;

		explicit iterator(std::span<const std::byte> rest)
		: rest_{rest} {
			parse_();
		}

		const DescriptorEntry &operator* () const {
			return cur_;
		}

		const DescriptorEntry *operator-> () const {
			return &cur_;
		}

		iterator &operator++ () {
			rest_ = rest_.subspan(cur_.first.length);
			parse_();
			return *this;
		}

		iterator operator++ (int) {
			auto copy = *this;
			++*this;
			return copy;
		}

		bool operator== (const iterator &other) const {
			return rest_.data() == other.rest_.data();
		}

		bool operator== (std::default_sentinel_t) const {
			return rest_.empty();
		}

	private:
		// Reads the current descriptor header and computes the entry. A header
		// that does not fit, or whose length would over-/under-run the buffer,
		// terminates iteration (rest_ is emptied so we compare equal to end()).
		void parse_() {
			if(rest_.size() < sizeof(DescriptorBase)) {
				rest_ = rest_.subspan(rest_.size());
				return;
			}
			DescriptorBase base;
			std::memcpy(&base, rest_.data(), sizeof(DescriptorBase));
			if(base.length < sizeof(DescriptorBase) || base.length > rest_.size()) {
				rest_ = rest_.subspan(rest_.size());
				return;
			}
			cur_ = DescriptorEntry{base, rest_.subspan(0, base.length)};
		}

		std::span<const std::byte> rest_;
		DescriptorEntry cur_;
	};

	ConfigurationView() = default;

	explicit ConfigurationView(std::span<const std::byte> buffer)
	: buffer_{buffer} { }

	iterator begin() const {
		return iterator{buffer_};
	}

	std::default_sentinel_t end() const {
		return {};
	}

	std::span<const std::byte> bytes() const {
		return buffer_;
	}

private:
	std::span<const std::byte> buffer_;
};

inline ConfigurationView configurationRange(std::span<const std::byte> buffer) {
	return ConfigurationView{buffer};
}

inline ConfigurationView configurationRange(std::string_view buffer) {
	return ConfigurationView{std::as_bytes(std::span{buffer.data(), buffer.size()})};
}

// Copies a descriptor out of an entry's byte span, validating that the span is
// large enough to hold the whole (fixed-size) descriptor. Returns std::nullopt
// for a truncated descriptor instead of reading out of bounds. Returning a copy
// also avoids reading through a potentially unaligned pointer into the buffer.
template<typename T>
std::optional<T> extractDescriptor(std::span<const std::byte> bytes) {
	if(bytes.size() < sizeof(T))
		return std::nullopt;
	std::array<std::byte, sizeof(T)> storage;
	std::memcpy(storage.data(), bytes.data(), sizeof(T));
	return std::bit_cast<T>(storage);
}

// The configuration descriptor is always the first entry of a config buffer.
inline std::optional<ConfigDescriptor> configDescriptorFrom(ConfigurationView cfg) {
	for(const auto &[head, body] : cfg) {
		if(head.descriptorType != descriptor_type::configuration)
			break;
		return extractDescriptor<ConfigDescriptor>(body);
	}
	return std::nullopt;
}

namespace _detail {
	inline bool isInterface(const DescriptorEntry &entry) {
		return entry.first.descriptorType == descriptor_type::interface;
	}

	inline bool isEndpoint(const DescriptorEntry &entry) {
		return entry.first.descriptorType == descriptor_type::endpoint;
	}

	// A truncated interface/endpoint descriptor degrades to a zeroed one rather
	// than reading out of bounds; the views above filter purely on the type byte.
	template<typename T>
	T descriptorValue(const DescriptorEntry &entry) {
		return extractDescriptor<T>(entry.second).value_or(T{});
	}
}

// Groups a configuration range into (InterfaceDescriptor, children) pairs. The
// children subrange covers the descriptors after the interface descriptor up to
// (excluding) the next interface descriptor.
template<std::ranges::forward_range R>
requires std::same_as<std::ranges::range_value_t<R>, DescriptorEntry>
auto groupByInterface(R cfg) {
	return std::forward<R>(cfg)
		| std::views::chunk_by([] (const DescriptorEntry &, const DescriptorEntry &next) {
			return !_detail::isInterface(next);
		})
		| std::views::filter([] (auto &&chunk) {
			return _detail::isInterface(*std::ranges::begin(chunk));
		})
		| std::views::transform([] (auto &&chunk) {
			auto desc = _detail::descriptorValue<InterfaceDescriptor>(*std::ranges::begin(chunk));
			return std::pair{desc, std::forward<decltype(chunk)>(chunk) | std::views::drop(1)};
		});
}

// Groups a configuration range into (EndpointDescriptor, children) pairs. The
// children subrange covers the descriptors after the endpoint descriptor up to
// the next endpoint or interface descriptor. Treating interface descriptors as
// boundaries (and discarding interface-led chunks) makes this correct whether
// it is applied to a whole configuration or to a single interface's children.
template<std::ranges::forward_range R>
requires std::same_as<std::ranges::range_value_t<R>, DescriptorEntry>
auto groupByEndpoint(R cfg) {
	return std::forward<R>(cfg)
		| std::views::chunk_by([] (const DescriptorEntry &, const DescriptorEntry &next) {
			return !_detail::isEndpoint(next) && !_detail::isInterface(next);
		})
		| std::views::filter([] (auto &&chunk) {
			return _detail::isEndpoint(*std::ranges::begin(chunk));
		})
		| std::views::transform([] (auto &&chunk) {
			auto desc = _detail::descriptorValue<EndpointDescriptor>(*std::ranges::begin(chunk));
			return std::pair{desc, std::forward<decltype(chunk)>(chunk) | std::views::drop(1)};
		});
}

// Yields the InterfaceDescriptors of a configuration range, without subranges.
template<std::ranges::forward_range R>
requires std::same_as<std::ranges::range_value_t<R>, DescriptorEntry>
auto interfacesOf(R cfg) {
	return std::forward<R>(cfg)
		| std::views::filter(_detail::isInterface)
		| std::views::transform(_detail::descriptorValue<InterfaceDescriptor>);
}

// Yields the EndpointDescriptors of a configuration range, without subranges.
template<std::ranges::forward_range R>
requires std::same_as<std::ranges::range_value_t<R>, DescriptorEntry>
auto endpointsOf(R cfg) {
	return std::forward<R>(cfg)
		| std::views::filter(_detail::isEndpoint)
		| std::views::transform(_detail::descriptorValue<EndpointDescriptor>);
}

} // namespace protocols::usb
