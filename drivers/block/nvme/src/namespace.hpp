#pragma once

#include <async/result.hpp>
#include <blockfs.hpp>

struct Controller;

struct Namespace : blockfs::BlockDevice {
    Namespace(Controller* controller, unsigned int nsid, int lbaShift);

    async::detached run();

    async::result<void> readSectors(uint64_t sector, void *buf, size_t numSectors) override;
    async::result<void> writeSectors(uint64_t sector, const void *buf, size_t numSectors) override;

private:
    Controller* controller_;
    unsigned int nsid_;
    int lbaShift_;
};
