#pragma once

#include <charconv>
#include <format>
#include <frg/expected.hpp>
#include <iostream>
#include <ranges>
#include <stdint.h>
#include <string>
#include <string.h>
#include <vector>

struct DevicePathParser {
	enum class Error {
		invalidNodeArgument,
	};

	static frg::expected<Error, DevicePathParser> fromString(std::string str) {
		std::vector<Node> nodes;
		auto parts = str | std::ranges::views::split('/') | std::ranges::views::transform([](auto &&s) { return std::string_view(&*s.begin(), std::ranges::distance(s)); });

		for(auto p : parts) {
			if(p.starts_with("PciRoot(")) {
				auto uid = p.substr(8, p.size() - 9);
				uint32_t uid_num = 0;
				auto uid_val = integerFromString<uint32_t>(uid);
				if(uid_val)
					uid_num = uid_val.value();

				Node n{
					.type = DevicePathType::ACPI,
					.subtype = 1,
					.data = {{ 0x41, 0xD0, 0x0A, 0x03 }},
				};

				n.data.resize(8);
				n.data.insert(n.data.end(), { 0x41, 0xD0, 0x0A, 0x03 });
				memcpy(&n.data[4], &uid_num, sizeof(uid_num));

				nodes.push_back(n);
			} else if(p.starts_with("Pci(")) {
				Node n{
					.type = DevicePathType::Hardware,
					.subtype = 1
				};

				auto slot = p.substr(4, p.find_first_of(',', 4) - 4);
				auto slot_num = FRG_TRY(integerFromString<uint8_t>(slot));
				n.data.push_back(slot_num);

				auto func = p.substr(p.find_first_of(',', 4) + 1, p.find_first_of(')', p.find_first_of(',', 4) + 1) - (p.find_first_of(',', 4) + 1));
				auto func_num = FRG_TRY(integerFromString<uint8_t>(func));
				n.data.push_back(func_num);

				nodes.push_back(n);
			} else {
				std::cout << std::format("core/device-path: unhandled device node type '{}', skipping", p) << std::endl;
			}
		}

		return DevicePathParser{std::move(nodes)};
	}

	std::string sysfs() {
		std::string path = "/sys/";

		for(auto &n : nodes_) {
			switch(n.type) {
				case DevicePathType::Hardware: {
					switch(n.subtype) {
						case 1: {
							path += std::format("0000:00:{:02x}.{:01x}/", n.data[0], n.data[1]);
							break;
						}
						default:
							std::cout << "core/device-path: unhandled hardware subtype" << std::endl;
							break;
					}
					break;
				}
				case DevicePathType::ACPI: {
					switch(n.subtype) {
						case 1: {
							char manufacturer_code[4];
							manufacturer_code[0] = ((n.data[0] & 0b01111100) >> 2) + 0x40;
							manufacturer_code[1] = (((n.data[0] & 0b00000011) << 3) | ((n.data[1] & 0b11100000) >> 5)) + 0x40;
							manufacturer_code[2] = (n.data[1] & 0b00011111) + 0x40;
							manufacturer_code[3] = '\0';
							uint16_t uid = 0;
							memcpy(&uid, &n.data[4], sizeof(uid));

							path += std::format("bus/acpi/devices/{}{:02X}{:02X}:{:02x}/physical_node/", manufacturer_code, n.data[2], n.data[3], uid);
							break;
						}
						default:
							std::cout << "core/device-path: unhandled ACPI subtype" << std::endl;
							break;
					}
					break;
				}
				default:
					std::cout << "core/device-path: unhandled devpath type" << std::endl;
					break;
			}
		}

		return path;
	}

private:
	enum class DevicePathType : uint8_t {
		Hardware = 1,
		ACPI = 2,
		Messaging = 3,
		MediaDevice = 4,
		BiosBootSpecification = 5,
	};

	struct Node {
		DevicePathType type;
		uint8_t subtype;
		std::vector<uint8_t> data;

		size_t size() const {
			return 4 + data.size();
		}
	};

	std::vector<Node> nodes_;

	DevicePathParser(std::vector<Node> nodes) : nodes_{std::move(nodes)} {}

	template<typename T>
	static frg::expected<Error, T> integerFromString(std::string_view str) {
		size_t base = 10;

		if(str.starts_with("0x")) {
			base = 16;
			str = str.substr(2);
		}

		T value;
		auto res = std::from_chars(str.begin(), str.end(), value, base);
		if(res.ec == std::errc::invalid_argument || res.ec == std::errc::result_out_of_range)
			return Error::invalidNodeArgument;

		return value;
	}
};
