#pragma once

#include <arch/register.hpp>
#include <cstddef>
#include <cstdint>

namespace nic::igc {

namespace reg {

inline constexpr arch::bit_register<uint32_t> ctrl{0x0000};
inline constexpr arch::bit_register<uint32_t> status{0x0008};
inline constexpr arch::bit_register<uint32_t> eecd{0x0010};
inline constexpr arch::bit_register<uint32_t> mdic{0x0020};
inline constexpr arch::bit_register<uint32_t> rctl{0x0100};
inline constexpr arch::bit_register<uint32_t> tctl{0x0400};
inline constexpr arch::bit_register<uint32_t> icr{0x1500};
inline constexpr arch::bit_register<uint32_t> ims{0x1508};
inline constexpr arch::scalar_register<uint32_t> imc{0x150C};
inline constexpr arch::bit_register<uint32_t> gpie{0x1514};
inline constexpr arch::scalar_register<uint32_t> eims{0x1524};
inline constexpr arch::scalar_register<uint32_t> eiac{0x152C};
inline constexpr arch::scalar_register<uint32_t> eiam{0x1530};
inline constexpr arch::scalar_register<uint32_t> eitr0{0x1680};
inline constexpr arch::bit_register<uint32_t> ivar0{0x1700};
inline constexpr arch::bit_register<uint32_t> ivarMisc{0x1740};
inline constexpr arch::bit_register<uint32_t> swsm{0x5B50};
inline constexpr arch::scalar_register<uint32_t> swFwSync{0x5B5C};

inline constexpr arch::scalar_register<uint32_t> rdbal0{0xC000};
inline constexpr arch::scalar_register<uint32_t> rdbah0{0xC004};
inline constexpr arch::scalar_register<uint32_t> rdlen0{0xC008};
inline constexpr arch::bit_register<uint32_t> srrctl0{0xC00C};
inline constexpr arch::scalar_register<uint32_t> rdh0{0xC010};
inline constexpr arch::scalar_register<uint32_t> rdt0{0xC018};
inline constexpr arch::bit_register<uint32_t> rxdctl0{0xC028};

inline constexpr arch::scalar_register<uint32_t> tdbal0{0xE000};
inline constexpr arch::scalar_register<uint32_t> tdbah0{0xE004};
inline constexpr arch::scalar_register<uint32_t> tdlen0{0xE008};
inline constexpr arch::scalar_register<uint32_t> tdh0{0xE010};
inline constexpr arch::scalar_register<uint32_t> tdt0{0xE018};
inline constexpr arch::bit_register<uint32_t> txdctl0{0xE028};

inline constexpr size_t rarCount = 16;
inline constexpr size_t mtaCount = 128;

inline constexpr auto ral(int n) { return arch::scalar_register<uint32_t>{0x5400 + 8 * n}; }
inline constexpr auto rah(int n) { return arch::scalar_register<uint32_t>{0x5404 + 8 * n}; }
inline constexpr auto mta(int i) { return arch::scalar_register<uint32_t>{0x5200 + 4 * i}; }

inline constexpr auto ral0 = ral(0);
inline constexpr auto rah0 = rah(0);

} // namespace reg

namespace ctrl {

inline constexpr arch::field<uint32_t, bool> gioMasterDisable{2, 1};
inline constexpr arch::field<uint32_t, bool> slu{6, 1};
inline constexpr arch::field<uint32_t, bool> frcspd{11, 1};
inline constexpr arch::field<uint32_t, bool> frcdpx{12, 1};
inline constexpr arch::field<uint32_t, bool> rst{26, 1};

} // namespace ctrl

namespace status {

inline constexpr arch::field<uint32_t, bool> fd{0, 1};
inline constexpr arch::field<uint32_t, bool> lu{1, 1};
inline constexpr arch::field<uint32_t, uint8_t> speed{6, 2};
inline constexpr arch::field<uint32_t, bool> gioMasterEnable{19, 1};
inline constexpr arch::field<uint32_t, bool> speed2500{22, 1};

} // namespace status

namespace eecd {

inline constexpr arch::field<uint32_t, bool> autoRd{9, 1};

} // namespace eecd

namespace mdic {

inline constexpr arch::field<uint32_t, uint16_t> data{0, 16};
inline constexpr arch::field<uint32_t, uint8_t> regAdd{16, 5};
inline constexpr arch::field<uint32_t, uint8_t> op{26, 2};
inline constexpr arch::field<uint32_t, bool> ready{28, 1};
inline constexpr arch::field<uint32_t, bool> error{30, 1};

inline constexpr uint8_t opWrite = 1;
inline constexpr uint8_t opRead = 2;

} // namespace mdic

namespace rctl {

inline constexpr arch::field<uint32_t, bool> en{1, 1};
inline constexpr arch::field<uint32_t, bool> bam{15, 1};
inline constexpr arch::field<uint32_t, bool> secrc{26, 1};

} // namespace rctl

namespace tctl {

inline constexpr arch::field<uint32_t, bool> en{1, 1};
inline constexpr arch::field<uint32_t, bool> psp{3, 1};
inline constexpr arch::field<uint32_t, uint8_t> ct{4, 8};
inline constexpr arch::field<uint32_t, bool> rtlc{24, 1};

inline constexpr uint8_t collisionThreshold = 15;

} // namespace tctl

namespace icr {

inline constexpr arch::field<uint32_t, bool> lsc{2, 1};

} // namespace icr

namespace ims {

inline constexpr arch::field<uint32_t, bool> lsc{2, 1};

} // namespace ims

namespace gpie {

inline constexpr arch::field<uint32_t, bool> nsicr{0, 1};
inline constexpr arch::field<uint32_t, bool> msixMode{4, 1};
inline constexpr arch::field<uint32_t, bool> eiame{30, 1};
inline constexpr arch::field<uint32_t, bool> pba{31, 1};

} // namespace gpie

inline constexpr uint32_t startItr = 648;

namespace ivar0 {

inline constexpr arch::field<uint32_t, uint8_t> rxQ0{0, 8};
inline constexpr arch::field<uint32_t, uint8_t> txQ0{8, 8};

} // namespace ivar0

namespace ivarMisc {

inline constexpr arch::field<uint32_t, uint8_t> other{8, 8};

} // namespace ivarMisc

inline constexpr uint8_t ivarValid = 0x80;

namespace rah {

inline constexpr arch::field<uint32_t, bool> mac4{0, 8};
inline constexpr arch::field<uint32_t, bool> mac5{8, 8};
inline constexpr arch::field<uint32_t, bool> av{31, 1};

} // namespace rah

namespace swsm {

inline constexpr arch::field<uint32_t, bool> smbi{0, 1};
inline constexpr arch::field<uint32_t, bool> swesmbi{1, 1};

} // namespace swsm

inline constexpr uint32_t swfwPhy0Sm = 0x2;

namespace srrctl0 {

inline constexpr arch::field<uint32_t, uint8_t> bsizepkt{0, 7};
inline constexpr arch::field<uint32_t, uint8_t> bsizehdr{8, 6};
inline constexpr arch::field<uint32_t, uint8_t> desctype{25, 3};

inline constexpr uint8_t desctypeAdvOnebuf = 1;

} // namespace srrctl0

namespace rxdctl0 {

inline constexpr arch::field<uint32_t, uint8_t> pthresh{0, 5};
inline constexpr arch::field<uint32_t, uint8_t> hthresh{8, 5};
inline constexpr arch::field<uint32_t, uint8_t> wthresh{16, 5};
inline constexpr arch::field<uint32_t, bool> queueEnable{25, 1};

} // namespace rxdctl0

namespace txdctl0 {

inline constexpr arch::field<uint32_t, uint8_t> pthresh{0, 5};
inline constexpr arch::field<uint32_t, uint8_t> hthresh{8, 5};
inline constexpr arch::field<uint32_t, uint8_t> wthresh{16, 5};
inline constexpr arch::field<uint32_t, bool> queueEnable{25, 1};

} // namespace txdctl0

namespace mii {

inline constexpr uint16_t crRestartAutoNeg = 0x0200;
inline constexpr uint16_t crPowerDown = 0x0800;
inline constexpr uint16_t crAutoNegEn = 0x1000;

} // namespace mii

namespace desc {

inline constexpr uint32_t advtxdDtypData = 0x0030'0000;
inline constexpr uint32_t advtxdDcmdEop = 0x0100'0000;
inline constexpr uint32_t advtxdDcmdIfcs = 0x0200'0000;
inline constexpr uint32_t advtxdDcmdRs = 0x0800'0000;
inline constexpr uint32_t advtxdDcmdDext = 0x2000'0000;
inline constexpr uint32_t advtxdPaylenShift = 14;

inline constexpr uint32_t txdStatDd = 1 << 0;
inline constexpr uint32_t rxdStatDd = 1 << 0;
inline constexpr uint32_t rxdStatEop = 1 << 1;

} // namespace desc

} // namespace nic::igc
