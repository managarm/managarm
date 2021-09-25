
#include <async/recurring-event.hpp>
#include <async/oneshot-event.hpp>
#include <async/result.hpp>
#include <blockfs.hpp>
#include <boost/intrusive/list.hpp>

enum Signatures {
	kSignCbw = 0x43425355,
	kSignCsw = 0x53425355
};

struct [[ gnu::packed ]] CommandBlockWrapper {
	uint32_t signature;
	uint32_t tag;
	uint32_t transferLength;
	uint8_t flags;
	uint8_t lun;
	uint8_t cmdLength;
	uint8_t cmdData[16];
};
static_assert(sizeof(CommandBlockWrapper) == 31);

struct [[ gnu::packed ]] CommandStatusWrapper {
	uint32_t signature;
	uint32_t tag;
	uint32_t dataResidue;
	uint8_t status;
};
static_assert(sizeof(CommandStatusWrapper) == 13);

namespace scsi {

struct Read6 {
	uint8_t opCode;
	uint8_t lba[3];
	uint8_t transferLength;
	uint8_t control;
};
static_assert(sizeof(Read6) == 6);

struct Read10 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[4];
	uint8_t groupNumber;
	uint8_t transferLength[2];
	uint8_t control;
};
static_assert(sizeof(Read10) == 10);

struct Write10 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[4];
	uint8_t groupNumber;
	uint8_t transferLength[2];
	uint8_t control;
};
static_assert(sizeof(Write10) == 10);

struct Read12 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[4];
	uint8_t transferLength[4];
	uint8_t grpNumber;
	uint8_t control;
};

struct Read16 {
	uint8_t opCode;
	uint8_t options;
	uint8_t lba[8];
	uint8_t transferLength[4];
	uint8_t grpNumber;
	uint8_t control;
};

struct Read32 {
	uint8_t opCode;
	uint8_t control;
	uint32_t no_use;
	uint8_t grpNumber;
	uint8_t cdbLength;
	uint8_t serviceAction[2];
	uint8_t options;
	uint8_t no_use2;
	uint8_t lba[8];
	uint8_t referenceTag[4];
	uint8_t applicationTag[2];
	uint8_t applicationTagMask[2];
	uint8_t transferLength[4];
};

} // namespace scsi

struct StorageDevice : blockfs::BlockDevice {
	StorageDevice(Device usb_device) 
	: blockfs::BlockDevice(512), _usbDevice(std::move(usb_device)) { }

	async::detached run(int config_num, int intf_num);

	async::result<void> readSectors(uint64_t sector,
			void *buffer, size_t numSectors) override;

	async::result<void> writeSectors(uint64_t sector,
			const void *buffer, size_t numSectors) override;

private:
	struct Request {
		Request(bool isWrite, uint64_t sector, void *buffer, size_t numSectors)
		: isWrite{isWrite}, sector{sector}, buffer{buffer}, numSectors{numSectors} { }

		bool isWrite;
		uint64_t sector;
		void *buffer;
		size_t numSectors;
		async::oneshot_event event;
		boost::intrusive::list_member_hook<> requestHook;
	};

	Device _usbDevice;
	async::recurring_event _doorbell;

	boost::intrusive::list<
		Request,
		boost::intrusive::member_hook<
			Request,
			boost::intrusive::list_member_hook<>,
			&Request::requestHook
		>
	> _queue;
};

