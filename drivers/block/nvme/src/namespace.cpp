#include <arch/bit.hpp>

#include "namespace.hpp"
#include "controller.hpp"

Namespace::Namespace(Controller* controller, unsigned int nsid, int lbaShift) : BlockDevice{(size_t)1 << lbaShift}, controller_(controller),
                                                                        nsid_(nsid), lbaShift_(lbaShift) {
}

async::detached Namespace::run() {
    blockfs::runDevice(this);

    co_return;
}

async::result<void> Namespace::readSectors(uint64_t sector, void *buffer, size_t numSectors) {
    using arch::convert_endian;
    using arch::endian;

    auto cmd = std::make_unique<Command>();
    auto& cmdBuf = cmd->getCommandBuffer().readWrite;

    cmdBuf.opcode = spec::kRead;
    cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid_);
    cmdBuf.startLba = convert_endian<endian::little, endian::native>(sector);
    cmdBuf.length = convert_endian<endian::little, endian::native>((uint16_t)numSectors - 1);
    cmd->setupBuffer(arch::dma_buffer_view{nullptr, buffer, numSectors << lbaShift_});

    return async::make_result(controller_->submitIoCommand(std::move(cmd)));
}

async::result<void> Namespace::writeSectors(uint64_t sector, const void *buffer, size_t numSectors) {
    using arch::convert_endian;
    using arch::endian;

    auto cmd = std::make_unique<Command>();
    auto& cmdBuf = cmd->getCommandBuffer().readWrite;

    cmdBuf.opcode = spec::kWrite;
    cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid_);
    cmdBuf.startLba = convert_endian<endian::little, endian::native>(sector);
    cmdBuf.length = convert_endian<endian::little, endian::native>((uint16_t)numSectors - 1);
    cmd->setupBuffer(arch::dma_buffer_view{nullptr, (char*)buffer, numSectors << lbaShift_});

    return async::make_result(controller_->submitIoCommand(std::move(cmd)));
}
