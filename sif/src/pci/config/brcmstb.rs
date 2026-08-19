use arch::{
    BitValue, IoMemSpace, MemAccess, Register, bit_register, runtime_bit_register,
    runtime_scalar_register, scalar_register,
};
use managarm::svrctl::hardware_access_handle;
use std::sync::Mutex;
use std::time::Duration;

use super::{ConfigIoError, PciConfigIo, Result, check_offset};
use crate::dt::node::DeviceTreeNode;

const CONFIG_SPACE_SIZE: u16 = 0x1000;

bit_register! {
    Lnksta @ 0x00be: u16 {
        LINK_SPEED @ 0, 4: u8;
        NEGOTIATED_LINK_WIDTH @ 4, 6: u8;
    }
}

scalar_register!(HwRev @ 0x406c: u32);

bit_register! {
    BridgeCtl @ 0x9210: u32 {
        RESET @ 0, 1: bool;
        SW_INIT @ 1, 1: bool;
    }
}

bit_register! {
    BridgeState @ 0x4068: u32 {
        PHY_ACTIVE @ 4, 1: bool;
        DL_ACTIVE @ 5, 1: bool;
        RC_MODE @ 7, 1: bool;
    }
}

bit_register! {
    HardDebug @ 0x4204: u32 {
        CLKREQ_ENABLE @ 1, 1: bool;
        SERDES_DISABLE @ 27, 1: bool;
    }
}

bit_register! {
    MiscCtl @ 0x4008: u32 {
        ACCESS_ENABLE @ 12, 1: bool;
        READ_UR_MODE @ 13, 1: bool;
        MAX_BURST_SIZE @ 20, 2: u8;
        SCB_SIZE0 @ 27, 5: u8;
    }
}

bit_register! {
    RcBar1Lo @ 0x402c: u32 {
        SIZE @ 0, 5: u8;
    }
}

bit_register! {
    RcBar2Lo @ 0x4034: u32 {
        SIZE @ 0, 5: u8;
    }
}

scalar_register!(RcBar2Hi @ 0x4038: u32);

bit_register! {
    RcBar3Lo @ 0x403c: u32 {
        SIZE @ 0, 5: u8;
    }
}

bit_register! {
    VendorReg1 @ 0x0188: u32 {
        ENDIAN_MODE @ 2, 2: u8;
    }
}

bit_register! {
    Priv1IdVal3 @ 0x043c: u32 {
        ID @ 0, 24: u32;
    }
}

bit_register! {
    Priv1LinkCap @ 0x04dc: u32 {
        LINK_CAP @ 10, 2: u8;
    }
}

bit_register! {
    CfgIndex @ 0x9000: u32 {
        FUNCTION @ 12, 3: u8;
        SLOT @ 15, 5: u8;
        BUS @ 20, 8: u8;
    }
}

const CFG_DATA: usize = 0x8000;

bit_register! {
    MdioAddr @ 0x1100: u32 {
        PKT_REG @ 0, 16: u16;
        PKT_PORT @ 16, 4: u8;
        PKT_CMD @ 20, 12: u16;
    }
}

bit_register! {
    MdioWrData @ 0x1104: u32 {
        DATA @ 0, 31: u32;
        DATA_DONE @ 31, 1: bool;
    }
}

bit_register! {
    MdioRdData @ 0x1108: u32 {
        DATA @ 0, 31: u32;
        DATA_DONE @ 31, 1: bool;
    }
}

runtime_scalar_register!(OutboundPcieLo: u32);
runtime_scalar_register!(OutboundPcieHi: u32);

runtime_bit_register! {
    OutboundBaseLimit: u32 {
        BASE @ 4, 12: u16;
        LIMIT @ 20, 12: u16;
    }
}

runtime_bit_register! {
    OutboundBaseHi: u32 {
        HI @ 0, 8: u8;
    }
}

runtime_bit_register! {
    OutboundLimitHi: u32 {
        HI @ 0, 8: u8;
    }
}

fn encode_rc_bar_size(size: u64) -> u8 {
    let n = 63 - size.leading_zeros();

    if (12..=15).contains(&n) {
        ((n - 12) + 0x1c) as u8
    } else if (16..=35).contains(&n) {
        (n - 15) as u8
    } else {
        0
    }
}

const fn link_speed_string(v: u8) -> &'static str {
    match v {
        0 => "down",
        1 => "2.5 GT/s",
        2 => "5.0 GT/s",
        4 => "8.0 GT/s",
        _ => "unknown",
    }
}

async fn sleep_for(duration: Duration) {
    hel::sleep_for(duration)
        .await
        .expect("sif: failed to sleep");
}

pub struct BrcmStbPcie {
    seg: u16,
    bus_start: u8,
    bus_end: u8,

    reg_space: IoMemSpace,
    // Configuration space is addressed through an index register, hence accesses to it have to
    // be serialized.
    mutex: Mutex<()>,
}

impl BrcmStbPcie {
    pub async fn new(
        node: &'static DeviceTreeNode,
        seg: u16,
        bus_start: u8,
        bus_end: u8,
    ) -> BrcmStbPcie {
        let addr = node.reg()[0].addr;
        let size = ((node.reg()[0].size + 0xFFF) & !0xFFF) as usize;

        let handle = hel::access_physical(
            hardware_access_handle(),
            addr as usize,
            size,
            hel::CachingMode::MmioNonPosted,
        )
        .expect("sif: failed to access the Broadcom STB PCIe registers");
        let mapping = unsafe {
            hel::Mapping::<u8>::new(
                &handle,
                None,
                0,
                size,
                hel::MappingFlags::READ | hel::MappingFlags::WRITE,
            )
        }
        .expect("sif: failed to map the Broadcom STB PCIe registers");
        // The mapping is never removed, hence the pointer stays valid after we leak it.
        let mapping = Box::leak(Box::new(mapping));

        let pcie = BrcmStbPcie {
            seg,
            bus_start,
            bus_end,
            reg_space: unsafe { IoMemSpace::new(mapping.as_ptr().unwrap().as_ptr(), size) },
            mutex: Mutex::new(()),
        };

        pcie.init().await;

        pcie
    }

    /// # Safety
    ///
    /// `reg` must be a controller-internal register; in particular it must not lie in the
    /// CFG_DATA window, which forwards to device configuration space.
    unsafe fn load<R: Register>(&self, reg: R) -> R::Repr
    where
        R::Bits: MemAccess,
    {
        unsafe { self.reg_space.load(reg) }
    }

    /// # Safety
    ///
    /// Like [`Self::load`]; additionally, stores to registers that program address
    /// translation (the RC BARs, SCB size and the outbound windows) must keep the windows
    /// consistent with the platform's address map: outbound windows must only claim CPU
    /// addresses that the SoC routes to this controller, and inbound windows must map PCIe
    /// addresses 1:1 onto RAM as DMA setup assumes.
    unsafe fn store<R: Register>(&self, reg: R, value: R::Repr)
    where
        R::Bits: MemAccess,
    {
        unsafe { self.reg_space.store(reg, value) };
    }

    async fn init(&self) {
        self.reset().await;

        let rev = unsafe { self.load(HwRev) } & 0xFFFF;
        println!("sif: BrcmStb revision: {rev:#x}");

        // Configure windows

        let v = unsafe { self.load(MiscCtl) };
        let v = v.with(MiscCtl::ACCESS_ENABLE, true);
        let v = v.with(MiscCtl::READ_UR_MODE, true);
        let v = v.with(MiscCtl::MAX_BURST_SIZE, /* 128 bytes */ 0);
        // SAFETY: read-modify-write that leaves SCB_SIZE0 untouched.
        unsafe { self.store(MiscCtl, v) };

        // TODO: read this out of the DT

        // SAFETY: mirrors thor's static RPi4 map: inbound RC BAR2 maps PCIe addresses 1:1
        // onto the first 8 GiB of the SCB, covering all of RAM as DMA setup assumes; SCB0's
        // size matches.
        unsafe {
            self.store(
                RcBar2Lo,
                BitValue::zero().with(RcBar2Lo::SIZE, encode_rc_bar_size(0x200000000)),
            );
            self.store(RcBar2Hi, 0);

            let v = self.load(MiscCtl);
            self.store(
                MiscCtl,
                v.with(
                    MiscCtl::SCB_SIZE0,
                    (63 - 0x200000000u64.leading_zeros() - 15) as u8,
                ),
            );
        }

        // SAFETY: size 0 disables the unused inbound BARs 1 and 3.
        unsafe {
            let v = self.load(RcBar1Lo);
            self.store(RcBar1Lo, v.with(RcBar1Lo::SIZE, 0));
            let v = self.load(RcBar3Lo);
            self.store(RcBar3Lo, v.with(RcBar3Lo::SIZE, 0));
        }

        self.enable().await;

        let mut i = 0;
        loop {
            let state = unsafe { self.load(BridgeState) };
            if state.get(BridgeState::DL_ACTIVE) && state.get(BridgeState::PHY_ACTIVE) {
                break;
            }

            sleep_for(Duration::from_millis(5)).await;

            if i >= 100 {
                panic!("sif: Bridge failed to start");
            }
            i += 1;
        }

        if !unsafe { self.load(BridgeState) }.get(BridgeState::RC_MODE) {
            panic!("sif: Bridge is in EP mode");
        }

        // TODO: read this out of the DT
        // SAFETY: mirrors thor's static RPi4 map: the SoC routes CPU addresses
        // 0x6_0000_0000..0x6_4000_0000 to this controller, and bus enumeration assigns BARs
        // from PCIe addresses starting at 0xC000_0000.
        unsafe { self.set_outbound_window(0, 0x600000000, 0xC0000000, 0x40000000) };

        unsafe {
            let v = self.load(Priv1LinkCap);
            self.store(Priv1LinkCap, v.with(Priv1LinkCap::LINK_CAP, 0b11)); // L1 & L0s

            let v = self.load(Priv1IdVal3);
            self.store(Priv1IdVal3, v.with(Priv1IdVal3::ID, 0x060400));
        }

        self.enable_ssc().await;

        let ls = unsafe { self.load(Lnksta) };
        println!(
            "sif: Link is up, speed {}, x{}",
            link_speed_string(ls.get(Lnksta::LINK_SPEED)),
            ls.get(Lnksta::NEGOTIATED_LINK_WIDTH)
        );

        unsafe {
            let v = self.load(VendorReg1);
            self.store(VendorReg1, v.with(VendorReg1::ENDIAN_MODE, 0));

            let v = self.load(HardDebug);
            self.store(HardDebug, v.with(HardDebug::CLKREQ_ENABLE, true));
        }
    }

    async fn reset(&self) {
        unsafe {
            let v = self.load(BridgeCtl);
            self.store(BridgeCtl, v.with(BridgeCtl::SW_INIT, true));
        }

        sleep_for(Duration::from_micros(200)).await;

        unsafe {
            let v = self.load(BridgeCtl);
            self.store(BridgeCtl, v.with(BridgeCtl::SW_INIT, false));
        }

        sleep_for(Duration::from_micros(200)).await;

        unsafe {
            let v = self.load(HardDebug);
            self.store(HardDebug, v.with(HardDebug::SERDES_DISABLE, false));
        }

        sleep_for(Duration::from_micros(100)).await;
    }

    async fn enable(&self) {
        unsafe {
            let v = self.load(BridgeCtl);
            self.store(BridgeCtl, v.with(BridgeCtl::RESET, false));
        }

        sleep_for(Duration::from_micros(100)).await;
    }

    /// # Safety
    ///
    /// `cpu_addr..cpu_addr + size` must be the outbound aperture that the SoC routes to this
    /// controller (in particular not backed by RAM), and `pcie_addr` must be the base of the
    /// PCIe address range that bus enumeration assigns BARs from.
    unsafe fn set_outbound_window(&self, n: usize, cpu_addr: u64, pcie_addr: u64, size: u64) {
        let pcie_lo = OutboundPcieLo::at(0x400c + n * 8);
        let pcie_hi = OutboundPcieHi::at(0x4010 + n * 8);

        // SAFETY, for all accesses below: the caller vouches for the window.
        unsafe {
            self.store(pcie_lo, pcie_addr as u32);
            self.store(pcie_hi, (pcie_addr >> 32) as u32);

            let base_limit = OutboundBaseLimit::at(0x4070 + n * 4);

            let base_mb = cpu_addr / 0x100000;
            let limit_mb = (cpu_addr + size - 1) / 0x100000;

            let v = self.load(base_limit);
            let v = v.with(OutboundBaseLimit::BASE, base_mb as u16);
            let v = v.with(OutboundBaseLimit::LIMIT, limit_mb as u16);
            self.store(base_limit, v);

            const HI_SHIFT: u64 = 12;

            let base_hi = OutboundBaseHi::at(0x4080 + n * 8);
            let limit_hi = OutboundLimitHi::at(0x4084 + n * 8);

            let v = self.load(base_hi);
            self.store(
                base_hi,
                v.with(OutboundBaseHi::HI, (base_mb >> HI_SHIFT) as u8),
            );
            let v = self.load(limit_hi);
            self.store(
                limit_hi,
                v.with(OutboundLimitHi::HI, (limit_mb >> HI_SHIFT) as u8),
            );
        }
    }

    async fn mdio_read(&self, port: u8, reg: u8) -> u32 {
        unsafe {
            self.store(
                MdioAddr,
                BitValue::zero()
                    .with(MdioAddr::PKT_PORT, port)
                    .with(MdioAddr::PKT_REG, reg as u16)
                    .with(MdioAddr::PKT_CMD, 1),
            );

            self.load(MdioAddr);
        }

        let mut tries = 0;
        loop {
            let data = unsafe { self.load(MdioRdData) };
            if data.get(MdioRdData::DATA_DONE) {
                return data.get(MdioRdData::DATA);
            }

            sleep_for(Duration::from_millis(10)).await;
            tries += 1;
            if tries > 10 {
                panic!("sif: MDIO read failure");
            }
        }
    }

    async fn mdio_write(&self, port: u8, reg: u8, val: u16) {
        unsafe {
            self.store(
                MdioAddr,
                BitValue::zero()
                    .with(MdioAddr::PKT_PORT, port)
                    .with(MdioAddr::PKT_REG, reg as u16)
                    .with(MdioAddr::PKT_CMD, 0),
            );

            self.load(MdioAddr);

            self.store(
                MdioWrData,
                BitValue::zero()
                    .with(MdioWrData::DATA_DONE, true)
                    .with(MdioWrData::DATA, val as u32),
            );
        }

        let mut tries = 0;
        loop {
            let data = unsafe { self.load(MdioWrData) };
            if !data.get(MdioWrData::DATA_DONE) {
                break;
            }

            sleep_for(Duration::from_millis(10)).await;
            tries += 1;
            if tries > 10 {
                panic!("sif: MDIO write failure");
            }
        }
    }

    async fn enable_ssc(&self) {
        self.mdio_write(0, 0x1f, 0x1100).await;
        let mut ctl = self.mdio_read(0, 0x0002).await;
        ctl |= 0x8000;
        ctl |= 0x4000;
        self.mdio_write(0, 0x0002, ctl as u16).await;

        sleep_for(Duration::from_millis(2)).await;

        let status = self.mdio_read(0, 0x0001).await;

        assert!((status & 0x400) != 0 && (status & 0x800) != 0);
    }

    fn check_address(&self, seg: u16, bus: u8, slot: u8, function: u8) -> Result<()> {
        if seg != self.seg
            || bus < self.bus_start
            || bus > self.bus_end
            || slot >= 32
            || function >= 8
        {
            return Err(ConfigIoError::InvalidAddress {
                seg,
                bus,
                slot,
                function,
            });
        }
        Ok(())
    }

    // Must be called while the config space mutex is held: it programs the index register.
    fn config_space_for(&self, bus: u8, slot: u8, function: u8) -> IoMemSpace {
        // Bus 0 accesses controller MMIO
        if bus == self.bus_start {
            assert!(slot == 0 && function == 0);
            return self.reg_space;
        }

        unsafe {
            self.store(
                CfgIndex,
                BitValue::zero()
                    .with(CfgIndex::BUS, bus)
                    .with(CfgIndex::SLOT, slot)
                    .with(CfgIndex::FUNCTION, function),
            )
        };
        self.reg_space.subspace(CFG_DATA)
    }
}

impl PciConfigIo for BrcmStbPcie {
    unsafe fn read_config_byte(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u8> {
        if bus == self.bus_start && (slot != 0 || function != 0) {
            return Ok(0xFF);
        }

        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 1, CONFIG_SPACE_SIZE)?;
        let _lock = self
            .mutex
            .lock()
            .expect("sif: Broadcom STB config space mutex was poisoned");
        let space = self.config_space_for(bus, slot, function);
        Ok(unsafe { space.scalar_load::<u8>(offset as usize) })
    }

    unsafe fn read_config_half(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u16> {
        if bus == self.bus_start && (slot != 0 || function != 0) {
            return Ok(0xFFFF);
        }

        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 2, CONFIG_SPACE_SIZE)?;
        let _lock = self
            .mutex
            .lock()
            .expect("sif: Broadcom STB config space mutex was poisoned");
        let space = self.config_space_for(bus, slot, function);
        Ok(unsafe { space.scalar_load::<u16>(offset as usize) })
    }

    unsafe fn read_config_word(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u32> {
        if bus == self.bus_start && (slot != 0 || function != 0) {
            return Ok(0xFFFFFFFF);
        }

        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 4, CONFIG_SPACE_SIZE)?;
        let _lock = self
            .mutex
            .lock()
            .expect("sif: Broadcom STB config space mutex was poisoned");
        let space = self.config_space_for(bus, slot, function);
        Ok(unsafe { space.scalar_load::<u32>(offset as usize) })
    }

    unsafe fn write_config_byte(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u8,
    ) -> Result<()> {
        if bus == self.bus_start && (slot != 0 || function != 0) {
            return Ok(());
        }

        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 1, CONFIG_SPACE_SIZE)?;
        let _lock = self
            .mutex
            .lock()
            .expect("sif: Broadcom STB config space mutex was poisoned");
        let space = self.config_space_for(bus, slot, function);
        unsafe { space.scalar_store::<u8>(offset as usize, value) };
        Ok(())
    }

    unsafe fn write_config_half(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u16,
    ) -> Result<()> {
        if bus == self.bus_start && (slot != 0 || function != 0) {
            return Ok(());
        }

        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 2, CONFIG_SPACE_SIZE)?;
        let _lock = self
            .mutex
            .lock()
            .expect("sif: Broadcom STB config space mutex was poisoned");
        let space = self.config_space_for(bus, slot, function);
        unsafe { space.scalar_store::<u16>(offset as usize, value) };
        Ok(())
    }

    unsafe fn write_config_word(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u32,
    ) -> Result<()> {
        if bus == self.bus_start && (slot != 0 || function != 0) {
            return Ok(());
        }

        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 4, CONFIG_SPACE_SIZE)?;
        let _lock = self
            .mutex
            .lock()
            .expect("sif: Broadcom STB config space mutex was poisoned");
        let space = self.config_space_for(bus, slot, function);
        unsafe { space.scalar_store::<u32>(offset as usize, value) };
        Ok(())
    }

    fn supports_4k_config_space(&self) -> bool {
        true
    }
}
