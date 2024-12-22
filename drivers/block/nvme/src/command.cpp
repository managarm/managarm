#include <arch/bit.hpp>
#include <helix/memory.hpp>
#include <unistd.h>

#include "command.hpp"

void Command::setupBuffer(arch::dma_buffer_view view, spec::DataTransfer policy) {
	using arch::convert_endian;
	using arch::endian;

	view_ = view;

	if(policy == spec::DataTransfer::PRP) {
		static size_t pageSize = getpagesize();

		uintptr_t virtStart = reinterpret_cast<uintptr_t>(view.data());
		auto offset = virtStart % pageSize;

		if (offset + view.size() <= pageSize * 2) {
			// Inline
			command_.common.dataPtr.prp.prp1 = convert_endian<endian::little, endian::native>(
				helix::ptrToPhysical(view.data()));

			auto firstPrpLen = pageSize - offset;
			if (view.size() > firstPrpLen) {
				command_.common.dataPtr.prp.prp2 = convert_endian<endian::little, endian::native>(
					helix::ptrToPhysical(view.subview(firstPrpLen).data()));
			}

			return;
		}

		uintptr_t prp1, prp2;
		auto size = view.size();

		prp1 = helix::addressToPhysical(virtStart);

		if (offset + view.size() <= pageSize) {
			command_.readWrite.dataPtr.prp.prp1 = convert_endian<endian::little, endian::native>(prp1);
			command_.readWrite.dataPtr.prp.prp2 = 0;
			return;
		}
		size -= pageSize - offset;
		virtStart += pageSize - offset;

		if (size <= pageSize) {
			command_.readWrite.dataPtr.prp.prp1 = convert_endian<endian::little, endian::native>(prp1);
			command_.readWrite.dataPtr.prp.prp2 = convert_endian<endian::little, endian::native>(
				helix::addressToPhysical(virtStart));
			return;
		}

		auto prpObj = arch::dma_array<uint64_t>{nullptr, pageSize >> 3};
		auto *prpList = prpObj.data();

		prp2 = helix::ptrToPhysical(prpList);
		prpLists.push_back(std::move(prpObj));

		int i = 0;
		for (;;) {
			if (i == pageSize >> 3) {
				auto *oldPrpList = prpList;
				prpObj = arch::dma_array<uint64_t>{nullptr, pageSize >> 3};
				prpList = prpObj.data();
				prpLists.push_back(std::move(prpObj));

				prpList[0] = oldPrpList[i - 1];
				oldPrpList[i - 1] = convert_endian<endian::little, endian::native>(
					helix::ptrToPhysical(prpList));
				i = 1;
			}
			prpList[i++] = convert_endian<endian::little, endian::native>(
				helix::addressToPhysical(virtStart));
			virtStart += pageSize;

			if (size <= pageSize)
				break;
			size -= pageSize;
		}

		command_.readWrite.dataPtr.prp.prp1 = convert_endian<endian::little, endian::native>(prp1);
		command_.readWrite.dataPtr.prp.prp2 = convert_endian<endian::little, endian::native>(prp2);
	}
}
