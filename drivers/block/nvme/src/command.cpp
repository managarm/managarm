#include <arch/bit.hpp>
#include <helix/ipc.hpp>
#include <unistd.h>

#include "command.hpp"

void Command::setupBuffer(arch::dma_buffer_view view) {
    using arch::convert_endian;
    using arch::endian;

    static size_t pageSize = getpagesize();

    uintptr_t virtStart = reinterpret_cast<uintptr_t>(view.data());
    auto offset = virtStart % pageSize;

    if (offset + view.size() <= pageSize * 2) {
        // Inline
        uintptr_t physical;
        HEL_CHECK(helPointerPhysical(view.data(), &physical));
        command_.common.dataPtr.prp1 = convert_endian<endian::little, endian::native>(physical);

        auto firstPrpLen = pageSize - offset;
        if (view.size() > firstPrpLen) {
            HEL_CHECK(helPointerPhysical(view.subview(firstPrpLen).data(), &physical));
            command_.common.dataPtr.prp2 = convert_endian<endian::little, endian::native>(physical);
        }

        return;
    }

    uintptr_t prp1, prp2;
    uintptr_t dmaAddr;
    auto size = view.size();

    HEL_CHECK(helPointerPhysical((void*)virtStart, &dmaAddr));
    prp1 = dmaAddr;

    if (offset + view.size() <= pageSize) {
        command_.readWrite.dataPtr.prp1 = convert_endian<endian::little, endian::native>(prp1);
        command_.readWrite.dataPtr.prp2 = 0;
        return;
    }
    size -= pageSize - offset;
    virtStart += pageSize - offset;
    HEL_CHECK(helPointerPhysical((void*)virtStart, &dmaAddr));

    if (size <= pageSize) {
        command_.readWrite.dataPtr.prp1 = convert_endian<endian::little, endian::native>(prp1);
        command_.readWrite.dataPtr.prp1 = convert_endian<endian::little, endian::native>(dmaAddr);
        return;
    }

    auto prpObj = arch::dma_array<uint64_t>{nullptr, pageSize >> 3};
    auto* prpList = prpObj.data();
    uintptr_t prpPhys;

    HEL_CHECK(helPointerPhysical(prpList, &prpPhys));
    prp2 = prpPhys;
    prpLists.push_back(std::move(prpObj));

    int i = 0;
    for (;;) {
        if (i == pageSize >> 3) {
            auto* oldPrpList = prpList;
            prpObj = arch::dma_array<uint64_t>{nullptr, pageSize >> 3};
            prpList = prpObj.data();
            HEL_CHECK(helPointerPhysical(prpList, &prpPhys));
            prpLists.push_back(std::move(prpObj));

            prpList[0] = oldPrpList[i - 1];
            oldPrpList[i - 1] = convert_endian<endian::little, endian::native>(prpPhys);
            i = 1;
        }
        prpList[i++] = convert_endian<endian::little, endian::native>(dmaAddr);
        virtStart += pageSize;
        HEL_CHECK(helPointerPhysical((void*)virtStart, &dmaAddr));

        if (size <= pageSize) break;
        size -= pageSize;
    }

    command_.readWrite.dataPtr.prp1 = convert_endian<endian::little, endian::native>(prp1);
    command_.readWrite.dataPtr.prp1 = convert_endian<endian::little, endian::native>(prp2);
}
