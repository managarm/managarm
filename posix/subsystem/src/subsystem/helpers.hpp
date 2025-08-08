#pragma once

#include <type_traits>

#include "../device.hpp"
#include "../sysfs.hpp"

template<class D>
requires std::is_base_of_v<UnixDevice, D>
struct DevAttribute : sysfs::Attribute {
	DevAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override {
		auto device = static_cast<D *>(object);
		auto dev = device->getId();
		// The format is 0:0\n\0.
		co_return std::to_string(dev.first) + ":" + std::to_string(dev.second) + "\n";
	}
};
