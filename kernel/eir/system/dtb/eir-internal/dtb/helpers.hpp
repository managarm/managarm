#pragma once

#include <expected>
#include <span>

#include <dtb.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/error.hpp>

namespace eir::dtb {

size_t addressCells(DeviceTreeNode node) {
	auto property = node.findProperty("#address-cells");
	if (!property)
		return 2; // DT specification: default is 2.
	uint32_t value;
	if (!property->read(value)) {
		infoLogger() << "eir: #address-cells of " << node.name() << " is broken" << frg::endlog;
		return 2;
	}
	return value;
}

size_t sizeCells(DeviceTreeNode node) {
	auto property = node.findProperty("#size-cells");
	if (!property)
		return 1; // DT specification: default is 1.
	uint32_t value;
	if (!property->read(value)) {
		infoLogger() << "eir: #size-cells of " << node.name() << " is broken" << frg::endlog;
		return 1;
	}
	return value;
}

// Translates a device address behind a bus (by recursively evaluating the ranges property).
std::expected<uint64_t, Error> translateAddress(uint64_t address, std::span<DeviceTreeNode> path) {
	if (path.empty()) {
		infoLogger() << "eir: Cannot translate address on empty path" << frg::endlog;
		return std::unexpected{Error::other};
	}

	while (path.size() > 1) {
		auto childNode = path[path.size() - 1];
		auto parentNode = path[path.size() - 2];
		path = path.subspan(0, path.size() - 1);

		auto parentAddressCells = addressCells(parentNode);
		auto childAddressCells = addressCells(childNode);
		auto childSizeCells = sizeCells(childNode);

		auto rangesProperty = childNode.findProperty("ranges");
		// DT specification: missing ranges means translation not possible.
		if (!rangesProperty) {
			infoLogger() << "eir: Cannot translate address to parent" << frg::endlog;
			return std::unexpected{Error::deviceInaccessible};
		}
		// DT specification: empty ranges means identity translation.
		if (!rangesProperty->size())
			continue;

		auto it = rangesProperty->access();
		std::optional<uint64_t> translated;
		while (it != ::dtb::endOfProperty) {
			uint64_t childAddress;
			uint64_t parentAddress;
			uint64_t size;
			if (!it.readCells(childAddress, childAddressCells)) {
				infoLogger() << "eir: Failed to read child address of ranges property"
				             << frg::endlog;
				return std::unexpected{Error::brokenBindings};
			}
			it += childAddressCells * sizeof(uint32_t);
			if (!it.readCells(parentAddress, parentAddressCells)) {
				infoLogger() << "eir: Failed to read parent address of ranges property"
				             << frg::endlog;
				return std::unexpected{Error::brokenBindings};
			}
			it += parentAddressCells * sizeof(uint32_t);
			if (!it.readCells(size, childSizeCells)) {
				infoLogger() << "eir: Failed to read size of ranges property" << frg::endlog;
				return std::unexpected{Error::brokenBindings};
			}
			it += childSizeCells * sizeof(uint32_t);

			// TODO: The addition here potentially needs to handle overflow.
			if (address >= childAddress && address < childAddress + size) {
				translated = address - childAddress + parentAddress;
				break;
			}
		}

		if (!translated) {
			// Address is not covered by the ranges property.
			return std::unexpected{Error::deviceInaccessible};
		}
		address = *translated;
	}

	return address;
}

} // namespace eir::dtb
