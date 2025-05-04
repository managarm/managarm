#pragma once

#include <blockfs.hpp>

#include <arch/dma_structs.hpp>
#include <async/recurring-event.hpp>
#include <frg/list.hpp>

#include <string>

namespace scsi {

enum class ErrorType {
	success,
	checkCondition,
	conditionMet,
	busy,
	reservationConflict,
	taskSetFull,
	acaActive,
	taskAborted,
	deviceSpecific
};

struct Error {
	constexpr bool operator==(const Error &other) const = default;

	std::string toString();

	ErrorType type;
	uint32_t code;
};

struct CommandInfo {
	arch::dma_buffer_view command;
	arch::dma_buffer_view data;
	bool isWrite;
};

Error statusToError(uint8_t status);

struct Interface {
	virtual ~Interface() = default;
	virtual async::result<frg::expected<Error, size_t>> sendScsiCommand(const CommandInfo &info) = 0;

	async::result<frg::expected<Error, std::vector<uint64_t>>> reportLuns();

	bool enableRead6{};
};

struct StorageDevice : Interface, blockfs::BlockDevice {
	StorageDevice(size_t sectorSize, int64_t parentId)
	: blockfs::BlockDevice(sectorSize, parentId) { }

	async::detached runScsi();

	async::result<void> readSectors(uint64_t sector,
			void *buffer, size_t numSectors) final;

	async::result<void> writeSectors(uint64_t sector,
			const void *buffer, size_t numSectors) final;

	async::result<size_t> getSize() final;

	size_t storageSize{};

private:
	struct Request {
		Request(bool isWrite, uint64_t sector, void *buffer, size_t numSectors)
		: isWrite{isWrite}, sector{sector}, buffer{buffer}, numSectors{numSectors} { }

		bool isWrite;
		uint64_t sector;
		void *buffer;
		size_t numSectors;
		async::oneshot_event event;
		frg::default_list_hook<Request> requestHook;
	};

	async::recurring_event doorbell_;

	frg::intrusive_list<
		Request,
		frg::locate_member<Request, frg::default_list_hook<Request>, &Request::requestHook>
	> queue_;
};

inline constexpr uint8_t WELL_KNOWN_REPORT_LUNS_LUN = 1;

} // namespace scsi
