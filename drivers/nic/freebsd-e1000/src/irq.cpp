#include <nic/freebsd-e1000/common.hpp>

async::detached E1000Nic::processIrqs() {
	co_await _device.enableBusIrq();

	// TODO: The kick here should not be required.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick, 0));

	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(_irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		auto status = E1000_READ_REG(&_hw, E1000_ICR);
		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, sequence));

		if(status & E1000_ICR_LSC) {
			printf("e1000: link up\n");
			status &= ~E1000_ICR_LSC;
		}

		/* Ignore TX queue empty and TX writeback interrupts for now */
		if(status & (E1000_ICR_TXQE | E1000_ICR_TXDW))
			status &= ~(E1000_ICR_TXQE | E1000_ICR_TXDW);

		if(status & E1000_ICR_RXT0) {
			printf("e1000: handling packet RX irq\n");
			while(eth_rx_pop());
			status &= ~E1000_ICR_RXT0;
		}

		status &= ~E1000_ICR_INT_ASSERTED;

		if(status)
			printf("e1000: unhandled IRQ status 0x%08x\n", status);
	}

	co_return;
}
