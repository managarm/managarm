use super::bindings;

#[derive(Default, Debug, Clone, Copy, PartialEq, Eq)]
pub enum IoType {
    #[default]
    None,
    Port,
    Memory,
}

impl IoType {
    fn decode(io_type: bindings::IoType) -> Self {
        match io_type {
            bindings::IoType::NoBar => IoType::None,
            bindings::IoType::Port => IoType::Port,
            bindings::IoType::Memory => IoType::Memory,
        }
    }
}

#[derive(Default, Debug, Clone, Copy)]
pub struct BarInfo {
    io_type: IoType,
    host_type: IoType,
    address: usize,
    length: usize,
    offset: u32,
}

impl BarInfo {
    fn decode(bar: &bindings::PciBar) -> Self {
        Self {
            io_type: IoType::decode(bar.io_type()),
            host_type: IoType::decode(bar.host_type()),
            address: bar.address() as usize,
            length: bar.length() as usize,
            offset: bar.offset(),
        }
    }

    pub fn io_type(&self) -> IoType {
        self.io_type
    }

    pub fn host_type(&self) -> IoType {
        self.host_type
    }

    pub fn address(&self) -> usize {
        self.address
    }

    pub fn length(&self) -> usize {
        self.length
    }

    pub fn offset(&self) -> u32 {
        self.offset
    }
}

#[derive(Default, Debug, Clone, Copy)]
pub struct ExpansionRomInfo {
    address: usize,
    length: usize,
}

impl ExpansionRomInfo {
    fn decode(rom: &bindings::PciExpansionRom) -> Self {
        Self {
            address: rom.address() as usize,
            length: rom.length() as usize,
        }
    }

    pub fn address(&self) -> usize {
        self.address
    }

    pub fn length(&self) -> usize {
        self.length
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Capability {
    type_: u32,
}

impl Capability {
    fn decode(cap: &bindings::PciCapability) -> Self {
        Self { type_: cap.type_() }
    }

    pub fn type_(&self) -> u32 {
        self.type_
    }
}

#[derive(Debug, Clone)]
pub struct PciInfo {
    bar_info: [BarInfo; 6],
    expansion_rom_info: ExpansionRomInfo,
    caps: Vec<Capability>,
    num_msis: u32,
    msi_x: bool,
}

impl PciInfo {
    pub(crate) fn decode(pci_info: &bindings::SvrResponse) -> Self {
        let pci_bars = pci_info.bars().unwrap_or_default();
        let caps = pci_info
            .capabilities()
            .unwrap_or_default()
            .iter()
            .map(Capability::decode)
            .collect();

        let mut bar_info = [BarInfo::default(); 6];

        for (i, pci_bar) in pci_bars.iter().enumerate() {
            bar_info[i] = BarInfo::decode(pci_bar);
        }

        Self {
            expansion_rom_info: pci_info
                .expansion_rom()
                .map(ExpansionRomInfo::decode)
                .unwrap_or_default(),
            num_msis: pci_info.num_msis().unwrap_or_default(),
            msi_x: pci_info.msi_x().unwrap_or_default() == 1,
            bar_info,
            caps,
        }
    }

    pub fn bar_info(&self) -> &[BarInfo; 6] {
        &self.bar_info
    }

    pub fn expansion_rom_info(&self) -> &ExpansionRomInfo {
        &self.expansion_rom_info
    }

    pub fn caps(&self) -> &[Capability] {
        &self.caps
    }

    pub fn num_msis(&self) -> u32 {
        self.num_msis
    }

    pub fn msi_x(&self) -> bool {
        self.msi_x
    }
}
