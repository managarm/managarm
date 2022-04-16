#include <iostream>
#include <arch/bits.hpp>
#include <arch/mem_space.hpp>
#include <nic/intel/e1000.hpp>

namespace regs {
    arch::bit_register<uint32_t> ctrl{0x0};
    arch::bit_register<uint32_t> status{0x8};
    arch::scalar_register<uint32_t> eeprom{0x14};
} // namespace regs

namespace flags {
    constexpr arch::field<uint32_t, bool> reset{26, 27};
} // namespace flags

namespace {
    struct e1000Nic : nic::Link {
        e1000Nic(protocols::hw::Device dev, helix::Mapping regs,
			helix::UniqueDescriptor bar);

	    virtual async::result<void> receive(arch::dma_buffer_view) override;
	    virtual async::result<void> send(const arch::dma_buffer_view) override;

        virtual ~e1000Nic() override = default;
	private:
        protocols::hw::Device device;
        helix::Mapping regSpace;
		helix::UniqueDescriptor e1000Bar;
        bool haveEEPROM = false;

        arch::contiguous_pool dmaPool_;
        arch::mem_space regs_;
        uint32_t read_eeprom(uint8_t offset);
    };

    uint32_t e1000Nic::read_eeprom(uint8_t offset) {
        uint32_t result = 0;

        if (haveEEPROM) {
            regs_.store(regs::eeprom, 1 | ((uint32_t)offset << 8));
            while (!((result = regs_.load(regs::eeprom)) & (1 << 4)));
        } else {
            regs_.store(regs::eeprom, 1 | ((uint32_t)offset << 2));
            while (!((result = regs_.load(regs::eeprom)) & (1 << 1)));
        }

        return static_cast<uint16_t>((result >> 16) & 0xFFFF);
    }

    e1000Nic::e1000Nic(protocols::hw::Device dev, helix::Mapping regs,
			helix::UniqueDescriptor bar) : nic::Link(1500, &dmaPool_), device{std::move(dev)},
            regSpace{std::move(regs)}, e1000Bar{std::move(bar)}, regs_{regSpace.get()} {
        
        // First off, reset the controller
        regs_.store(regs::ctrl, flags::reset(true));
        while ((regs_.load(regs::ctrl) & flags::reset) != 0);

        // Then, see if eeprom is supported
        regs_.store(regs::eeprom, 0x01);
        for (size_t i = 0; i < 1000 && !haveEEPROM; i++) {
            if (regs_.load(regs::eeprom) & 0x10) {
                haveEEPROM = true;
                break;
            } else {
                haveEEPROM = false;
            }
        }
        if (!haveEEPROM)
            std::cout << "\e[31m" "intel-e1000: NIC does not support EEPROM!!!" "\e[39m\n";

        // Read in the MAC address
        if (haveEEPROM) {
            uint32_t tmp = read_eeprom(0);
            mac_[0] = tmp & 0xFF;
            mac_[1] = tmp >> 8;
            tmp = read_eeprom(1);
            mac_[2] = tmp & 0xFF;
            mac_[3] = tmp >> 8;
            tmp = read_eeprom(2);
            mac_[4] = tmp & 0xFF;
            mac_[5] = tmp >> 8;
        } else {
            for (int i = 0; i < 5; i++) {
                // TODO(cleanbaja): find a less hacky way to do this
                arch::scalar_register<uint32_t> macAddress{0x5400 + i};
                mac_[i] = regs_.load(macAddress);
            }
        }

        // Print the MAC address, which the e1000 always has
        char ms[3 * 6 + 1];
        sprintf(ms, "%.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
                mac_[0], mac_[1], mac_[2],
                mac_[3], mac_[4], mac_[5]);
        std::cout << "intel-e1000: Device has a hardware MAC: "
            << ms << std::endl;
    }

    async::result<void> e1000Nic::receive(arch::dma_buffer_view) {
        co_return;
    }
	async::result<void> e1000Nic::send(const arch::dma_buffer_view) {
        co_return;
    }

}

namespace nic::e1000 {
    std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device, helix::Mapping regSpace,
			helix::UniqueDescriptor bar) {
        return std::make_shared<e1000Nic>(std::move(device), std::move(regSpace), std::move(bar));
    }
}
