#pragma once

#include <async/mutex.hpp>
#include <frg/spinlock.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/hypervisor-space.hpp>
#include <thor-internal/virtualization.hpp>

#include <atomic>
#include <optional>

namespace thor::riscv_hypervisor {

void init();

struct Vcpu final : VirtualizedCpu {
	Vcpu(smarter::shared_ptr<HypervisorSpace> space);
	~Vcpu();

	Vcpu(const Vcpu &) = delete;
	Vcpu &operator=(const Vcpu &) = delete;

	frg::expected<Error, HelVmexitReason> run() override;

	void storeRegs(const HelVirtualizationRegs *regs) override;
	void loadRegs(HelVirtualizationRegs *res) override;

	bool assertInterrupt(uint64_t number, bool level) override;

private:
	void restoreHypervisorCsrs_();
	void saveHypervisorCsrs_();

	bool handleException_(HelVmexitReason &exitReason);
	bool handlePageFault_(HelVmexitReason &exitReason, uint32_t flags);

	std::optional<uint32_t> readInstruction_(uintptr_t address);

	smarter::shared_ptr<HypervisorSpace> space_;
	HelVirtualizationRegs state_{};
	alignas(8) char fpState_[Executor::fpStateSize]{};
	frg::ticket_spinlock irqInjectionMutex_{};
	uint64_t hvip_{};
	CpuData *runningCpu_{};
	async::mutex mutex_{};

	uint64_t scause_{};
	uint64_t stval_{};
	uint64_t htval_{};
	uint64_t htinst_{};
};

} // namespace thor::riscv_hypervisor
