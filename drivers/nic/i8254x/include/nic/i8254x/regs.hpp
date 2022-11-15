#pragma once

#include <arch/mem_space.hpp>

namespace regs {
	constexpr arch::bit_register<uint32_t> ctrl{0x00};
	constexpr arch::bit_register<uint32_t> status{0x08};
	constexpr arch::bit_register<uint32_t> eecd{0x10};
	constexpr arch::bit_register<uint32_t> eerd{0x14};
	constexpr arch::scalar_register<uint32_t> fcal{0x28};
	constexpr arch::scalar_register<uint32_t> fcah{0x2C};
	constexpr arch::scalar_register<uint32_t> fct{0x30};
	constexpr arch::bit_register<uint32_t> icr{0xC0};
	constexpr arch::scalar_register<uint32_t> ims{0xD0};
	constexpr arch::bit_register<uint32_t> rctl{0x100};
	constexpr arch::scalar_register<uint32_t> fcttv{0x170};
	constexpr arch::bit_register<uint32_t> tctl{0x400};
	constexpr arch::bit_register<uint32_t> tipg{0x410};
	constexpr arch::scalar_register<uint32_t> rdbal{0x2800};
	constexpr arch::scalar_register<uint32_t> rdbah{0x2804};
	constexpr arch::scalar_register<uint32_t> rdlen{0x2808};
	constexpr arch::scalar_register<uint32_t> rdh{0x2810};
	constexpr arch::scalar_register<uint32_t> rdt{0x2818};
	constexpr arch::scalar_register<uint32_t> tdbal{0x3800};
	constexpr arch::scalar_register<uint32_t> tdbah{0x3804};
	constexpr arch::scalar_register<uint32_t> tdlen{0x3808};
	constexpr arch::scalar_register<uint32_t> tdh{0x3810};
	constexpr arch::scalar_register<uint32_t> tdt{0x3818};
	constexpr arch::scalar_register<uint32_t> mta{0x5200};
	constexpr arch::scalar_register<uint32_t> ral_0{0x5400};
	constexpr arch::scalar_register<uint32_t> rah_0{0x5404};
} // namespace regs

namespace flags {
	namespace ctrl {
		constexpr arch::field<uint32_t, bool> set_link_up{6, 1};
		constexpr arch::field<uint32_t, bool> lrst{3, 1};
		constexpr arch::field<uint32_t, bool> asde{5, 1};
		constexpr arch::field<uint32_t, bool> ilos{7, 1};
		constexpr arch::field<uint32_t, bool> reset{26, 1};
		constexpr arch::field<uint32_t, bool> vme{30, 1};
		constexpr arch::field<uint32_t, bool> phy_reset{31, 1};
	}

	namespace eecd {
		constexpr arch::field<uint32_t, bool> present{8, 1};
	}

	namespace eerd {
		constexpr arch::field<uint32_t, bool> start{0, 1};
		constexpr arch::field<uint32_t, bool> done{1, 1};
		constexpr arch::field<uint32_t, uint8_t> addr{2, 14};
		constexpr arch::field<uint32_t, uint16_t> data{16, 16};
	}

	namespace rctl {
		constexpr arch::field<uint32_t, bool> receiver_enable{1, 1};
		constexpr arch::field<uint32_t, bool> store_bad_packets{2, 1};
		constexpr arch::field<uint32_t, bool> unicast_promiscuous{3, 1};
		constexpr arch::field<uint32_t, bool> multicast_promiscuous{4, 1};
		constexpr arch::field<uint32_t, bool> long_packet_reception{5, 1};
		constexpr arch::field<uint32_t, uint8_t> loopback_mode{6, 2};
		constexpr arch::field<uint32_t, uint8_t> rdmts{8, 2};
		constexpr arch::field<uint32_t, bool> broadcast_accept{15, 1};
		constexpr arch::field<uint32_t, uint8_t> receive_buffer_size{16, 2};
		constexpr arch::field<uint32_t, bool> bsex{25, 1};
		constexpr arch::field<uint32_t, bool> strip_ethernet_crc{26, 1};
	}
} // namespace flags
