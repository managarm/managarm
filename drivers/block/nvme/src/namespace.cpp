#include <arch/bit.hpp>
#include <asm/ioctl.h>
#include <format>
#include <linux/nvme_ioctl.h>

#include "namespace.hpp"
#include "controller.hpp"

Namespace::Namespace(Controller *controller, unsigned int nsid, int lbaShift)
	: BlockDevice{(size_t)1 << lbaShift, -1}, controller_(controller), nsid_(nsid),
	  lbaShift_(lbaShift) {
	diskNamePrefix = "nvme";
	diskNameSuffix = std::format("n{}", nsid);
	partNameSuffix = std::format("n{}p", nsid);
}

async::detached Namespace::run() {
	mbus_ng::Properties descriptor{
		{"class", mbus_ng::StringItem{"nvme-namespace"}},
		{"nvme.nsid", mbus_ng::StringItem{std::to_string(nsid_)}},
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(controller_->getMbusId())}},
	};

	mbusEntity_ = std::make_unique<mbus_ng::EntityManager>((co_await mbus_ng::Instance::global().createEntity(
		"nvme-namespace", descriptor)).unwrap());
	parentId = mbusEntity_->id();

	blockfs::runDevice(this);

	co_return;
}

async::result<void> Namespace::readSectors(uint64_t sector, void *buffer, size_t numSectors) {
	using arch::convert_endian;
	using arch::endian;

	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().readWrite;

	cmdBuf.opcode = spec::kRead;
	cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid_);
	cmdBuf.startLba = convert_endian<endian::little, endian::native>(sector);
	cmdBuf.length = convert_endian<endian::little, endian::native>((uint16_t)numSectors - 1);
	cmd->setupBuffer(arch::dma_buffer_view{nullptr, buffer, numSectors << lbaShift_}, controller_->dataTransferPolicy());

	co_await controller_->submitIoCommand(std::move(cmd));
}

async::result<void> Namespace::writeSectors(uint64_t sector, const void *buffer, size_t numSectors) {
	using arch::convert_endian;
	using arch::endian;

	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().readWrite;

	cmdBuf.opcode = spec::kWrite;
	cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid_);
	cmdBuf.startLba = convert_endian<endian::little, endian::native>(sector);
	cmdBuf.length = convert_endian<endian::little, endian::native>((uint16_t)numSectors - 1);
	cmd->setupBuffer(arch::dma_buffer_view{nullptr, (char *)buffer, numSectors << lbaShift_}, controller_->dataTransferPolicy());

	co_await controller_->submitIoCommand(std::move(cmd));
}

async::result<size_t> Namespace::getSize() {
	std::cout << "nvme: Namespace::getSize() is a stub!" << std::endl;
	co_return 1;
}
