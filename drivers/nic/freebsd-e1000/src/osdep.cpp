#include <async/basic.hpp>
#include <frg/container_of.hpp>
#include <nic/freebsd-e1000/common.hpp>
#include <protocols/hw/client.hpp>

struct e1000_pci {
	protocols::hw::Device &pci;
};

void e1000_osdep_set_pci(struct e1000_osdep *st, protocols::hw::Device &pci) {
	struct e1000_pci tmp{.pci = reinterpret_cast<protocols::hw::Device &>(pci)};
	st->pci = &tmp;
}

void e1000_write_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value) {
	auto run = [hw, reg, value]() -> async::result<void> {
		co_await hw2pci(hw).storePciSpace(reg, 2, *value);
		co_return;
	};
	async::run(run(), helix::currentDispatcher);
}

void e1000_read_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value) {
	hw2nic(hw)->pciRead(reg, value);
}

void e1000_pci_set_mwi(struct e1000_hw *hw [[gnu::unused]]) {
	assert(!"PANIC: e1000_pci_set_mwi unimplemented");
}

void e1000_pci_clear_mwi(struct e1000_hw *hw [[gnu::unused]]) {
	assert(!"PANIC: e1000_pci_clear_mwi unimplemented");
}

void e1000_io_write(struct e1000_hw *hw, u16 reg, u32 data) {
	auto nic = (frg::container_of(hw, &E1000Nic::_hw));
	nic->_io.store(arch::scalar_register<u32>(hw2iobase(hw) + reg), data);
}

namespace {

/*
 * Code in this anonymous namespace is adapted from ACRN.
 * Source:
 * https://github.com/projectacrn/acrn-hypervisor/blob/eb8bcb06b362781d0ab24918336d7fcd61b5abe3/devicemodel/hw/pci/pci_util.c
 *
 * Copyright (C) 2021-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#define PCIR_STATUS 0x06U
#define PCIM_STATUS_CAPPRESENT 0x0010U
#define PCICAP_ID 0x0U
#define PCICAP_NEXTPTR 0x1U
#define PCIR_CAP_PTR 0x34U
#define PCIY_EXPRESS 0x10

int pci_find_cap(struct e1000_hw *hw, const int cap_id) {
	uint8_t cap_pos, cap_data;
	uint16_t status = 0;

	hw2nic(hw)->pciRead(PCIR_STATUS, &status);

	if (status & PCIM_STATUS_CAPPRESENT) {
		hw2nic(hw)->pciRead(PCIR_CAP_PTR, &cap_pos);

		while (cap_pos != 0 && cap_pos != 0xff) {
			hw2nic(hw)->pciRead(cap_pos + PCICAP_ID, &cap_data);

			if (cap_data == cap_id)
				return cap_pos;

			hw2nic(hw)->pciRead(cap_pos + PCICAP_NEXTPTR, &cap_pos);
		}
	}

	return 0;
}

} // namespace

int32_t e1000_read_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value) {
	u32 offset = pci_find_cap(hw, PCIY_EXPRESS);
	e1000_read_pci_cfg(hw, offset + reg, value);

	return E1000_SUCCESS;
}

int32_t e1000_write_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value) {
	u32 offset = pci_find_cap(hw, PCIY_EXPRESS);
	e1000_write_pci_cfg(hw, offset + reg, value);

	return E1000_SUCCESS;
}
