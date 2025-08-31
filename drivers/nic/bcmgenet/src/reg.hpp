#pragma once

#include <cstdint>
#include <arch/mem_space.hpp>
#include <arch/register.hpp>

namespace nic::bcmgenet {

namespace reg {

inline constexpr arch::bit_register<uint32_t> rev{0x000};
inline constexpr arch::scalar_register<uint32_t> portCtrl{0x004};
inline constexpr arch::scalar_register<uint32_t> rbufFlush{0x008};
inline constexpr arch::bit_register<uint32_t> tbufFlush{0x00c};
inline constexpr arch::bit_register<uint32_t> extRgmiiOob{0x08c};

namespace intr {

inline constexpr arch::bit_register<uint32_t> stat{0x200};
inline constexpr arch::bit_register<uint32_t> clear{0x208};
inline constexpr arch::bit_register<uint32_t> statMask{0x20c};
inline constexpr arch::bit_register<uint32_t> setMask{0x210};
inline constexpr arch::bit_register<uint32_t> clearMask{0x214};

} // namespace intr

inline constexpr arch::bit_register<uint32_t> rbufCtrl{0x300};
inline constexpr arch::scalar_register<uint32_t> bufSize{0x3b4};

namespace umac {

inline constexpr arch::bit_register<uint32_t> cmd{0x808};
inline constexpr arch::scalar_register<uint32_t> mac0{0x80c};
inline constexpr arch::scalar_register<uint32_t> mac1{0x810};
inline constexpr arch::scalar_register<uint32_t> maxFrameLen{0x814};
inline constexpr arch::scalar_register<uint32_t> txFlush{0xb34};
inline constexpr arch::bit_register<uint32_t> mibCtrl{0xd80};
inline constexpr arch::bit_register<uint32_t> mdf{0xe50};
inline constexpr auto mdfAddrLo(int idx) {
	return arch::scalar_register<uint32_t>{0xe54 + idx * 8};
}
inline constexpr auto mdfAddrHi(int idx) {
	return arch::scalar_register<uint32_t>{0xe58 + idx * 8};
}

} // namespace umac

namespace mdio {

inline constexpr arch::bit_register<uint32_t> cmd{0xe14};

} // namespace mdio

inline constexpr ptrdiff_t rxBase = 0x2000;
inline constexpr ptrdiff_t txBase = 0x4000;
inline constexpr size_t ringSize = 0x40;
inline constexpr size_t descSize = 12;

namespace rxDma {

inline constexpr arch::mem_space subspace(arch::mem_space space, int qid) {
	return space.subspace(rxBase + 0xc00 + qid * ringSize);
}

inline constexpr arch::scalar_register<uint32_t> writePtrLo{0x00};
inline constexpr arch::scalar_register<uint32_t> writePtrHi{0x04};
inline constexpr arch::scalar_register<uint32_t> prodIndex{0x08};
inline constexpr arch::scalar_register<uint32_t> consIndex{0x0c};
inline constexpr arch::bit_register<uint32_t> ringBufSize{0x10};
inline constexpr arch::scalar_register<uint32_t> startAddrLo{0x14};
inline constexpr arch::scalar_register<uint32_t> startAddrHi{0x18};
inline constexpr arch::scalar_register<uint32_t> endAddrLo{0x1c};
inline constexpr arch::scalar_register<uint32_t> endAddrHi{0x20};
inline constexpr arch::scalar_register<uint32_t> mbufDoneThres{0x24};
inline constexpr arch::bit_register<uint32_t> xonXoffThres{0x28};
inline constexpr arch::scalar_register<uint32_t> readPtrLo{0x2c};
inline constexpr arch::scalar_register<uint32_t> readPtrHi{0x30};

inline constexpr arch::bit_register<uint32_t> ringCfg{rxBase + 0x1040};
inline constexpr arch::bit_register<uint32_t> ctrl{rxBase + 0x1044};
inline constexpr arch::scalar_register<uint32_t> scbBurstSize{rxBase + 0x104c};
inline constexpr auto ringTimeout(int qid) {
	return arch::bit_register<uint32_t>{rxBase + 0x106c + qid * 4};
}

} // namespace rxDma

namespace txDma {

inline constexpr arch::mem_space subspace(arch::mem_space space, int qid) {
	return space.subspace(txBase + 0xc00 + qid * ringSize);
}

inline constexpr arch::scalar_register<uint32_t> readPtrLo{0x00};
inline constexpr arch::scalar_register<uint32_t> readPtrHi{0x04};
inline constexpr arch::scalar_register<uint32_t> consIndex{0x08};
inline constexpr arch::scalar_register<uint32_t> prodIndex{0x0c};
inline constexpr arch::bit_register<uint32_t> ringBufSize{0x10};
inline constexpr arch::scalar_register<uint32_t> startAddrLo{0x14};
inline constexpr arch::scalar_register<uint32_t> startAddrHi{0x18};
inline constexpr arch::scalar_register<uint32_t> endAddrLo{0x1c};
inline constexpr arch::scalar_register<uint32_t> endAddrHi{0x20};
inline constexpr arch::scalar_register<uint32_t> mbufDoneThres{0x24};
inline constexpr arch::scalar_register<uint32_t> flowPeriod{0x28};
inline constexpr arch::scalar_register<uint32_t> writePtrLo{0x2c};
inline constexpr arch::scalar_register<uint32_t> writePtrHi{0x30};

inline constexpr arch::bit_register<uint32_t> ringCfg{txBase + 0x1040};
inline constexpr arch::bit_register<uint32_t> ctrl{txBase + 0x1044};
inline constexpr arch::scalar_register<uint32_t> scbBurstSize{txBase + 0x104c};
inline constexpr auto ringTimeout(int qid) {
	return arch::bit_register<uint32_t>{txBase + 0x106c + qid * 4};
}

} // namespace txDma

namespace desc {

inline constexpr arch::mem_space rxSubspace(arch::mem_space space, int idx) {
	return space.subspace(rxBase + 0x000 + idx * descSize);
}

inline constexpr arch::mem_space txSubspace(arch::mem_space space, int idx) {
	return space.subspace(txBase + 0x000 + idx * descSize);
}

inline constexpr arch::bit_register<uint32_t> status{0x00};
inline constexpr arch::scalar_register<uint32_t> addrLo{0x04};
inline constexpr arch::scalar_register<uint32_t> addrHi{0x08};

} // namespace desc

} // namespace reg


namespace rev {

inline constexpr arch::field<uint32_t, uint8_t> minor{16, 4};
inline constexpr arch::field<uint32_t, uint8_t> major{24, 4};

} // namespace rev


namespace portCtrl {

inline constexpr uint32_t extGphy = 0x00000003;

} // namespace portCtrl


namespace rbufCtrl {

inline constexpr arch::field<uint32_t, bool> reset{0, 1};
inline constexpr arch::field<uint32_t, bool> align2b{1, 1};

} // namespace rbufCtrl


namespace extRgmiiOob {

inline constexpr arch::field<uint32_t, bool> rgmiiIdDisable{16, 1};
inline constexpr arch::field<uint32_t, bool> rgmiiMode{6, 1};
inline constexpr arch::field<uint32_t, bool> oobDisable{5, 1};
inline constexpr arch::field<uint32_t, bool> rgmiiLink{4, 1};

} // namespace extRgmiiOob


namespace intr {

inline constexpr arch::field<uint32_t, bool> rxDmaDone{13, 1};
inline constexpr arch::field<uint32_t, bool> txDmaDone{16, 1};
inline constexpr arch::field<uint32_t, bool> mdioDone{23, 1};
inline constexpr arch::field<uint32_t, bool> mdioError{24, 1};

} // namespace intr


namespace umac {

namespace cmd {

inline constexpr arch::field<uint32_t, bool> swReset{13, 1};
inline constexpr arch::field<uint32_t, bool> localLoopback{15, 1};
inline constexpr arch::field<uint32_t, uint8_t> speed{2, 2};
inline constexpr arch::field<uint32_t, bool> txEnable{0, 1};
inline constexpr arch::field<uint32_t, bool> rxEnable{1, 1};
inline constexpr arch::field<uint32_t, bool> promisc{4, 1};

} // namespace cmd

namespace mibCtrl {

inline constexpr arch::field<uint32_t, bool> resetRunt{1, 1};
inline constexpr arch::field<uint32_t, bool> resetRx{0, 1};
inline constexpr arch::field<uint32_t, bool> resetTx{2, 1};

} // namespace mibCtrl

} // namespace umac


namespace ring {

inline constexpr arch::field<uint32_t, uint16_t> xonXoffThresHi{0, 16};
inline constexpr arch::field<uint32_t, uint16_t> xonXoffThresLo{16, 16};

inline constexpr arch::field<uint32_t, uint16_t> descCount{16, 16};
inline constexpr arch::field<uint32_t, uint16_t> bufLength{0, 16};

inline constexpr arch::field<uint32_t, bool> enable{0, 1};
inline constexpr arch::field<uint32_t, uint8_t> intrThreshold{0, 8};
inline constexpr arch::field<uint32_t, uint16_t> ringTimeout{0, 15};

} // namespace ring


namespace desc {

inline constexpr arch::field<uint32_t, uint16_t> buflen{16, 12};
inline constexpr arch::field<uint32_t, bool> own{15, 1};
inline constexpr arch::field<uint32_t, bool> eop{14, 1};
inline constexpr arch::field<uint32_t, bool> sop{13, 1};

namespace rx {

inline constexpr arch::field<uint32_t, bool> overrunErr{0, 1};
inline constexpr arch::field<uint32_t, bool> crcErr{1, 1};
inline constexpr arch::field<uint32_t, bool> rxErr{2, 1};
inline constexpr arch::field<uint32_t, bool> frameErr{3, 1};
inline constexpr arch::field<uint32_t, bool> lenErr{4, 1};

inline constexpr arch::field<uint32_t, uint8_t> allErrors{0, 5};

} // namespace rx

namespace tx {

inline constexpr arch::field<uint32_t, bool> crc{6, 1};
inline constexpr arch::field<uint32_t, uint8_t> qtag{7, 6};

} // namespace tx

} // namespace desc


namespace mdio::cmd {

inline constexpr arch::field<uint32_t, bool> startBusy{29, 1};
inline constexpr arch::field<uint32_t, bool> read{27, 1};
inline constexpr arch::field<uint32_t, bool> write{26, 1};
inline constexpr arch::field<uint32_t, uint8_t> pmd{21, 5};
inline constexpr arch::field<uint32_t, uint8_t> reg{16, 5};
inline constexpr arch::field<uint32_t, uint16_t> data{0, 16};

} // mdio::cmd


} // namespace nic::bcmgenet
