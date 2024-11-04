#include "raw.hpp"

namespace blockfs {
namespace raw {

RawFs::RawFs(BlockDevice *device) : device{device} {}

async::result<void> RawFs::init() {
	auto device_size = co_await device->getSize();
	auto cache_size = (device_size + 0xFFF) & ~size_t(0xFFF);
	HEL_CHECK(helCreateManagedMemory(cache_size, 0, &backingMemory, &frontalMemory));

	manageMapping();
	co_return;
}

async::detached RawFs::manageMapping() {
	while (true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(
		    helix::BorrowedDescriptor{backingMemory}, &manage, helix::Dispatcher::global()
		);
		co_await submit.async_wait();
		HEL_CHECK(manage.error());

		auto device_size = co_await device->getSize();
		auto cache_size = (device_size + 0xFFF) & ~size_t(0xFFF);
		assert(manage.offset() + manage.length() <= cache_size);

		if (manage.type() == kHelManageInitialize) {
			helix::Mapping file_map{
			    helix::BorrowedDescriptor{backingMemory},
			    static_cast<ptrdiff_t>(manage.offset()),
			    manage.length(),
			    kHelMapProtWrite
			};
			assert(!(manage.offset() & device->sectorSize));

			size_t backed_size = std::min(manage.length(), device_size - manage.offset());
			size_t num_blocks = (backed_size + device->sectorSize - 1) / device->sectorSize;

			assert(num_blocks * device->sectorSize <= manage.length());
			co_await device->readSectors(
			    manage.offset() / device->sectorSize, file_map.get(), num_blocks
			);

			HEL_CHECK(helUpdateMemory(
			    backingMemory, kHelManageInitialize, manage.offset(), manage.length()
			));
		} else {
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping file_map{
			    helix::BorrowedDescriptor{backingMemory},
			    static_cast<ptrdiff_t>(manage.offset()),
			    manage.length(),
			    kHelMapProtRead
			};

			assert(!(manage.offset() & device->sectorSize));

			size_t backed_size = std::min(manage.length(), device_size - manage.offset());
			size_t num_blocks = (backed_size + device->sectorSize - 1) / device->sectorSize;

			assert(num_blocks * device->sectorSize <= manage.length());
			co_await device->writeSectors(
			    manage.offset() / device->sectorSize, file_map.get(), num_blocks
			);

			HEL_CHECK(helUpdateMemory(
			    backingMemory, kHelManageWriteback, manage.offset(), manage.length()
			));
		}
	}
}

OpenFile::OpenFile(RawFs *rawFs) : rawFs(rawFs) {}

} // namespace raw
} // namespace blockfs
