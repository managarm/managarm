#pragma once

#include <async/result.hpp>
#include <protocols/mbus/client.hpp>
#include <blockfs.hpp>
#include <protocols/fs/common.hpp>

struct Controller;

struct Namespace : blockfs::BlockDevice {
	Namespace(Controller *controller, unsigned int nsid, int lbaShift, size_t lbaCount);

	async::detached run();

	async::result<void> readSectors(uint64_t sector, arch::dma_buffer_view view) override;
	async::result<void> writeSectors(uint64_t sector, arch::dma_buffer_view view) override;
	async::result<size_t> getSize() override;

	async::result<void> handleIoctl(managarm::fs::GenericIoctlRequest &req, helix::BorrowedDescriptor conversation) override;

private:
	Controller *controller_;
	unsigned int nsid_;
	int lbaShift_;
	size_t lbaCount_;
	std::unique_ptr<mbus_ng::EntityManager> mbusEntity_;
};
