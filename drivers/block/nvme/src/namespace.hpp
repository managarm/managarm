#pragma once

#include <async/result.hpp>
#include <protocols/mbus/client.hpp>
#include <blockfs.hpp>
#include <protocols/fs/common.hpp>

struct Controller;

struct Namespace : blockfs::BlockDevice {
	Namespace(Controller *controller, unsigned int nsid, int lbaShift);

	async::detached run();

	async::result<void> readSectors(uint64_t sector, void *buf, size_t numSectors) override;
	async::result<void> writeSectors(uint64_t sector, const void *buf, size_t numSectors) override;
	async::result<size_t> getSize() override;

	async::result<void> handleIoctl(managarm::fs::GenericIoctlRequest &req, helix::UniqueDescriptor conversation) override;

private:
	Controller *controller_;
	unsigned int nsid_;
	int lbaShift_;
	std::unique_ptr<mbus_ng::EntityManager> mbusEntity_;
};
