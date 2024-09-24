/* This file is derived from fuchsia's `fuchsia.c` and has been adapted for use in managarm.
 * The license is preserved below. For the original file, look here:
 * https://fuchsia.googlesource.com/fuchsia/+/a10b58da9f068289236785bd2f707142ca13798a/zircon/third_party/dev/ethernet/e1000/fuchsia.c
 */

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2016 Nicole Graziano <nicole@nextbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <nic/freebsd-e1000/common.hpp>

#define TARC_SPEED_MODE_BIT (1 << 21) /* On PCI-E MACs only */
#define TARC_ERRATA_BIT (1 << 26)     /* Note from errata on 82574 */

async::result<void> E1000Nic::txInit() {
	uint64_t bus_addr = helix::ptrToPhysical(&_txd[0]);

	/* Base and Len of TX Ring */
	E1000_WRITE_REG(&_hw, E1000_TDLEN(0), TX_QUEUE_SIZE * sizeof(struct e1000_tx_desc));
	E1000_WRITE_REG(&_hw, E1000_TDBAH(0), (u32)(bus_addr >> 32));
	E1000_WRITE_REG(&_hw, E1000_TDBAL(0), (u32)bus_addr);
	/* Init the HEAD/TAIL indices */
	E1000_WRITE_REG(&_hw, E1000_TDT(0), 0);
	E1000_WRITE_REG(&_hw, E1000_TDH(0), 0);

	u32 txdctl = 0; /* clear txdctl */
	txdctl |= 0x1f; /* PTHRESH */
	txdctl |= 1 << 8; /* HTHRESH */
	txdctl |= 1 << 16; /* WTHRESH */
	txdctl |= 1 << 22; /* Reserved bit 22 must always be 1 */
	txdctl |= E1000_TXDCTL_GRAN;
	txdctl |= 1 << 25; /* LWTHRESH */

	E1000_WRITE_REG(&_hw, E1000_TXDCTL(0), txdctl);

	u32 tipg = 0;

	/* Set the default values for the Tx Inter Packet Gap timer */
	switch (_hw.mac.type) {
		case e1000_80003es2lan:
			tipg = DEFAULT_82543_TIPG_IPGR1;
			tipg |= DEFAULT_80003ES2LAN_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
			break;
		case e1000_82542:
			tipg = DEFAULT_82542_TIPG_IPGT;
			tipg |= DEFAULT_82542_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
			tipg |= DEFAULT_82542_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
			break;
		default:
			if ((_hw.phy.media_type == e1000_media_type_fiber) ||
			(_hw.phy.media_type == e1000_media_type_internal_serdes))
				tipg = DEFAULT_82543_TIPG_IPGT_FIBER;
			else
				tipg = DEFAULT_82543_TIPG_IPGT_COPPER;
			tipg |= DEFAULT_82543_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
			tipg |= DEFAULT_82543_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
	}

	E1000_WRITE_REG(&_hw, E1000_TIPG, tipg);
	E1000_WRITE_REG(&_hw, E1000_TIDV, 0);

	if (_hw.mac.type >= e1000_82540)
		E1000_WRITE_REG(&_hw, E1000_TADV, 0);

	u32 tarc = 0;

	if ((_hw.mac.type == e1000_82571) || (_hw.mac.type == e1000_82572)) {
		tarc = E1000_READ_REG(&_hw, E1000_TARC(0));
		tarc |= TARC_SPEED_MODE_BIT;
		E1000_WRITE_REG(&_hw, E1000_TARC(0), tarc);
	} else if (_hw.mac.type == e1000_80003es2lan) {
		/* errata: program both queues to unweighted RR */
		tarc = E1000_READ_REG(&_hw, E1000_TARC(0));
		tarc |= 1;
		E1000_WRITE_REG(&_hw, E1000_TARC(0), tarc);
		tarc = E1000_READ_REG(&_hw, E1000_TARC(1));
		tarc |= 1;
		E1000_WRITE_REG(&_hw, E1000_TARC(1), tarc);
	} else if (_hw.mac.type == e1000_82574) {
		tarc = E1000_READ_REG(&_hw, E1000_TARC(0));
		tarc |= TARC_ERRATA_BIT;
		E1000_WRITE_REG(&_hw, E1000_TARC(0), tarc);
	}

	/* Program the Transmit Control Register */
	u32 tctl = E1000_READ_REG(&_hw, E1000_TCTL);
	tctl &= ~E1000_TCTL_CT;
	tctl |= (E1000_TCTL_PSP | E1000_TCTL_RTLC | E1000_TCTL_EN | (E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT));

	if (_hw.mac.type >= e1000_82571)
		tctl |= E1000_TCTL_MULR;

	/* This write will effectively turn on the transmit unit. */
	E1000_WRITE_REG(&_hw, E1000_TCTL, tctl);
	printf("e1000: TCTL enabled\n");

	/* SPT and KBL errata workarounds */
	if (_hw.mac.type == e1000_pch_spt) {
		u32 reg;
		reg = E1000_READ_REG(&_hw, E1000_IOSFPC);
		reg |= E1000_RCTL_RDMTS_HEX;
		E1000_WRITE_REG(&_hw, E1000_IOSFPC, reg);
		/* i218-i219 Specification Update 1.5.4.5 */
		reg = E1000_READ_REG(&_hw, E1000_TARC(0));
		reg &= ~E1000_TARC0_CB_MULTIQ_3_REQ;
		reg |= E1000_TARC0_CB_MULTIQ_2_REQ;
		E1000_WRITE_REG(&_hw, E1000_TARC(0), reg);
	}

	co_return;
}

void E1000Nic::reap_tx_buffers() {
	auto n = _txIndex;

	for (;;) {
		struct e1000_tx_desc* desc = &_txd[n];

		if(!(desc->upper.fields.status & E1000_TXD_STAT_DD)) {
			break;
		}

		desc->upper.fields.status = 0;
		++n;
	}

	_txIndex = n;
}
