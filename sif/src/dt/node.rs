//! Semantic device tree; port of thor's system/dtb/dtb.cpp.

use std::collections::BTreeMap;
use std::sync::{Mutex, OnceLock};

use anyhow::{Context, Result};
use managarm::svrctl::hardware_access_handle;

use crate::EXPECT_LOCK;
use crate::acpi::PAGE_MASK;
use crate::dt::DtError;
use crate::dt::fdt;
use crate::dt::irq::IrqController;

static PHANDLES: Mutex<BTreeMap<u32, &'static DeviceTreeNode>> = Mutex::new(BTreeMap::new());
static TREE_ROOT: OnceLock<&'static DeviceTreeNode> = OnceLock::new();

pub struct RegRange {
    pub addr: u64,
    pub size: u64,
}

pub struct BusRange {
    pub from: u32,
    pub to: u32,
}

pub struct AddrTranslateRange {
    pub child_addr_hi: u32,
    pub child_addr: u64,
    pub parent_addr: u64,
    pub size: u64,

    pub child_addr_hi_valid: bool,
}

pub struct DeviceTreeNode {
    dt_node: fdt::DeviceTreeNode<'static>,

    parent: Option<&'static DeviceTreeNode>,

    children: Mutex<Vec<&'static DeviceTreeNode>>,

    name: &'static str,
    path: String,
    model: &'static str,
    phandle: u32,
    compatible: Vec<&'static str>,

    reg: Vec<RegRange>,
    ranges: Vec<AddrTranslateRange>,

    interrupt_controller: bool,

    // Objects associated with this DeviceTreeNode.
    associated_irq_controller: OnceLock<&'static dyn IrqController>,
}

fn parse_string_list(prop: fdt::DeviceTreeProperty<'static>) -> Vec<&'static str> {
    let mut list = Vec::new();

    let mut i = 0;
    while i < prop.size() {
        let sv = fdt::read_string_at(prop.data(), i);
        i += sv.len() + 1;
        list.push(sv);
    }

    list
}

// Reads the next value of a property that is num_cells cells wide.
fn read_value(
    it: &mut fdt::Accessor<'static>,
    num_cells: usize,
    name: &'static str,
) -> Result<u64, DtError> {
    let cells = it
        .into_cells(num_cells)
        .ok_or(DtError::TruncatedProperty { name })?;
    it.advance(num_cells * size_of::<u32>());
    cells.read().ok_or(DtError::UnsupportedCells {
        name,
        cells: num_cells,
    })
}

// Like read_value(), but splits off the high cell of PCI(e) addresses, which are 3 cells long.
fn read_address(
    it: &mut fdt::Accessor<'static>,
    num_cells: usize,
    name: &'static str,
) -> Result<(Option<u32>, u64), DtError> {
    if num_cells != 3 {
        return Ok((None, read_value(it, num_cells, name)?));
    }
    let hi = read_value(it, 1, name)? as u32;
    Ok((Some(hi), read_value(it, 2, name)?))
}

impl DeviceTreeNode {
    fn new(
        dt_node: fdt::DeviceTreeNode<'static>,
        parent: Option<&'static DeviceTreeNode>,
    ) -> Result<&'static DeviceTreeNode, DtError> {
        let name = dt_node.name();

        let mut node = DeviceTreeNode {
            dt_node,
            parent,
            children: Mutex::new(Vec::new()),
            name,
            path: Self::generate_path(name, parent),
            model: "",
            phandle: 0,
            compatible: Vec::new(),
            reg: Vec::new(),
            ranges: Vec::new(),
            interrupt_controller: false,
            associated_irq_controller: OnceLock::new(),
        };

        if let Some(phandle) = node.u32_property("phandle")? {
            node.phandle = phandle;
        } else if let Some(phandle) = node.u32_property("linux,phandle")? {
            println!("sif: warning: node \"{name}\" uses legacy \"linux,phandle\" property!");
            node.phandle = phandle;
        }

        for prop in dt_node.properties() {
            match prop.name() {
                "model" => {
                    node.model = fdt::read_string_at(prop.data(), 0);
                }
                "compatible" => {
                    node.compatible = parse_string_list(prop);
                }
                "interrupt-controller" => {
                    node.interrupt_controller = true;
                }
                "reg" if prop.size() != 0 => {
                    let parent = parent.ok_or(DtError::PropertyOnRoot { name: "reg" })?;
                    let addr_cells = parent.address_cells()?;
                    let size_cells = parent.size_cells()?;
                    if addr_cells + size_cells == 0 {
                        return Err(DtError::ZeroSizedEntries { name: "reg" });
                    }

                    let mut it = prop.access();
                    while !it.at_end_of_property() {
                        let (_, addr) = read_address(&mut it, addr_cells, "reg")?;
                        let size = read_value(&mut it, size_cells, "reg")?;
                        node.reg.push(RegRange { addr, size });
                    }
                }
                "ranges" if prop.size() != 0 => {
                    let parent_addr_cells = parent
                        .ok_or(DtError::PropertyOnRoot { name: "ranges" })?
                        .address_cells()?;
                    let child_addr_cells = node.address_cells()?;
                    let size_cells = node.size_cells()?;
                    if child_addr_cells + parent_addr_cells + size_cells == 0 {
                        return Err(DtError::ZeroSizedEntries { name: "ranges" });
                    }

                    let mut it = prop.access();
                    while !it.at_end_of_property() {
                        let (child_addr_hi, child_addr) =
                            read_address(&mut it, child_addr_cells, "ranges")?;
                        let parent_addr = read_value(&mut it, parent_addr_cells, "ranges")?;
                        let size = read_value(&mut it, size_cells, "ranges")?;
                        node.ranges.push(AddrTranslateRange {
                            child_addr_hi: child_addr_hi.unwrap_or(0),
                            child_addr,
                            parent_addr,
                            size,
                            child_addr_hi_valid: child_addr_hi.is_some(),
                        });
                    }
                }
                _ => {}
            }
        }

        // Unlike thor, addresses are translated at construction time; this is equivalent
        // since parents are always fully initialized before their children.
        if let Some(parent) = parent
            && !parent.ranges.is_empty()
        {
            for r in &mut node.reg {
                r.addr = parent.translate_address(r.addr)?;
            }

            for r in &mut node.ranges {
                r.parent_addr = parent.translate_address(r.parent_addr)?;
            }
        }

        let node: &'static DeviceTreeNode = Box::leak(Box::new(node));
        if node.phandle != 0 {
            PHANDLES
                .lock()
                .expect(EXPECT_LOCK)
                .insert(node.phandle, node);
        }
        Ok(node)
    }

    fn generate_path(name: &str, parent: Option<&'static DeviceTreeNode>) -> String {
        let mut components = vec![name];

        let mut p = parent;
        while let Some(node) = p {
            components.push(node.name);
            p = node.parent;
        }

        let mut path = String::new();
        for component in components.iter().rev() {
            if !component.is_empty() {
                path += "/";
            }
            path += component;
        }
        path
    }

    fn u32_property(&self, name: &'static str) -> Result<Option<u32>, DtError> {
        let Some(prop) = self.dt_node.find_property(name) else {
            return Ok(None);
        };
        let mut it = prop.access();
        Ok(Some(read_value(&mut it, 1, name)? as u32))
    }

    // Cell counts are read on demand, such that a malformed one only fails the code that needs it.
    fn address_cells(&self) -> Result<usize, DtError> {
        Ok(self.u32_property("#address-cells")?.unwrap_or(2) as usize)
    }

    fn size_cells(&self) -> Result<usize, DtError> {
        Ok(self.u32_property("#size-cells")?.unwrap_or(1) as usize)
    }

    fn interrupt_cells(&self) -> Result<usize, DtError> {
        Ok(self.u32_property("#interrupt-cells")?.unwrap_or(0) as usize)
    }

    fn attach_child(&self, node: &'static DeviceTreeNode) {
        self.children.lock().expect(EXPECT_LOCK).push(node);
    }

    pub fn dt_node(&self) -> &fdt::DeviceTreeNode<'static> {
        &self.dt_node
    }

    pub fn name(&self) -> &'static str {
        self.name
    }

    pub fn parent(&self) -> Option<&'static DeviceTreeNode> {
        self.parent
    }

    pub fn compatible(&self) -> &[&'static str] {
        &self.compatible
    }

    /// Returns the interrupt parent, which nodes without an interrupt-parent property inherit.
    pub fn interrupt_parent(&self) -> Result<&'static DeviceTreeNode, DtError> {
        let mut node = self;
        loop {
            if let Some(phandle) = node.u32_property("interrupt-parent")? {
                return get_device_tree_node_by_phandle(phandle)
                    .ok_or(DtError::DanglingPhandle { phandle });
            }

            let parent = node.parent.ok_or(DtError::NoInterruptParent)?;
            if parent.is_interrupt_controller() {
                // thor identifies interrupt controllers by their phandle.
                if parent.phandle == 0 {
                    return Err(DtError::InterruptControllerWithoutPhandle {
                        controller: parent.path.clone(),
                    });
                }
                return Ok(parent);
            }
            node = parent;
        }
    }

    pub fn path(&self) -> &str {
        &self.path
    }

    pub fn phandle(&self) -> u32 {
        self.phandle
    }

    pub fn is_compatible(&self, with: &[&str]) -> bool {
        self.compatible.iter().any(|c| with.contains(c))
    }

    pub fn is_interrupt_controller(&self) -> bool {
        self.interrupt_controller
    }

    pub fn reg(&self) -> &[RegRange] {
        &self.reg
    }

    pub fn ranges(&self) -> &[AddrTranslateRange] {
        &self.ranges
    }

    pub fn bus_range(&self) -> Result<BusRange, DtError> {
        let Some(prop) = self.dt_node.find_property("bus-range") else {
            return Ok(BusRange { from: 0, to: 0xFF });
        };
        let mut it = prop.access();
        Ok(BusRange {
            from: read_value(&mut it, 1, "bus-range")? as u32,
            to: read_value(&mut it, 1, "bus-range")? as u32,
        })
    }

    pub fn associate_irq_controller(&self, controller: &'static dyn IrqController) {
        assert!(
            self.associated_irq_controller.set(controller).is_ok(),
            "sif: node \"{}\" already has an associated IRQ controller",
            self.path
        );
    }

    pub fn associated_irq_controller(&self) -> Option<&'static dyn IrqController> {
        self.associated_irq_controller.get().copied()
    }

    pub fn translate_address(&self, addr: u64) -> Result<u64, DtError> {
        // We only handle simple bus address translation.
        if !self.is_compatible(&["simple-bus"]) {
            return Ok(addr);
        }

        // This node has no translation table.
        if self.ranges.is_empty() {
            return Ok(addr);
        }

        for tr in &self.ranges {
            if addr >= tr.child_addr && addr < tr.child_addr + tr.size {
                return Ok(tr.parent_addr + (addr - tr.child_addr));
            }
        }

        Err(DtError::AddressNotInRanges { address: addr })
    }

    pub fn for_each(&'static self, f: &mut impl FnMut(&'static DeviceTreeNode) -> bool) -> bool {
        let children = self.children.lock().expect(EXPECT_LOCK).clone();
        for child in children {
            if f(child) {
                return true;
            }
            if child.for_each(f) {
                return true;
            }
        }

        false
    }
}

pub fn get_device_tree_node_by_phandle(phandle: u32) -> Option<&'static DeviceTreeNode> {
    PHANDLES.lock().expect(EXPECT_LOCK).get(&phandle).copied()
}

pub fn get_device_tree_root() -> Option<&'static DeviceTreeNode> {
    TREE_ROOT.get().copied()
}

/// Maps the device tree blob and builds the semantic tree from it.
pub fn init(address: u64, size: u64) -> Result<()> {
    let page_off = address as usize & PAGE_MASK;
    let aligned = address as usize & !PAGE_MASK;
    let span = (size as usize + page_off + PAGE_MASK) & !PAGE_MASK;

    let handle = hel::access_physical(
        hardware_access_handle(),
        aligned,
        span,
        hel::CachingMode::Default,
    )
    .context("failed to access the device tree blob")?;
    let mapping =
        unsafe { hel::Mapping::<u8>::new(&handle, None, 0, span, hel::MappingFlags::READ) }
            .context("failed to map the device tree blob")?;
    let mapping = Box::leak(Box::new(mapping));
    let base = unsafe { mapping.as_ptr() }
        .context("device tree mapping has no address")?
        .as_ptr();
    let data: &'static [u8] =
        unsafe { std::slice::from_raw_parts(base.add(page_off), size as usize) };

    let tree: &'static fdt::DeviceTree = Box::leak(Box::new(fdt::DeviceTree::new(data)));

    let root = DeviceTreeNode::new(tree.root_node(), None).context("cannot use the root node")?;
    assert!(
        TREE_ROOT.set(root).is_ok(),
        "sif: device tree was already initialized"
    );

    println!("sif: Booting on \"{}\"", root.model);

    struct Walker {
        curr: Option<&'static DeviceTreeNode>,
        // Depth within the subtree of a node that could not be used, which is dropped.
        skipped_depth: usize,
    }

    impl fdt::DeviceTreeWalker<'static> for Walker {
        fn push(&mut self, dt_node: fdt::DeviceTreeNode<'static>) {
            if self.skipped_depth > 0 {
                self.skipped_depth += 1;
                return;
            }

            let curr = self.curr.expect("sif: device tree walker escaped the root");
            match DeviceTreeNode::new(dt_node, Some(curr)) {
                Ok(node) => {
                    curr.attach_child(node);
                    self.curr = Some(node);
                }
                Err(err) => {
                    println!(
                        "sif: Ignoring DT node {} and its children: {err}",
                        DeviceTreeNode::generate_path(dt_node.name(), Some(curr))
                    );
                    self.skipped_depth = 1;
                }
            }
        }

        fn pop(&mut self) {
            if self.skipped_depth > 0 {
                self.skipped_depth -= 1;
                return;
            }

            self.curr = self
                .curr
                .expect("sif: device tree walker escaped the root")
                .parent;
        }
    }

    let mut walker = Walker {
        curr: Some(root),
        skipped_depth: 0,
    };
    tree.root_node().walk_children(&mut walker);

    Ok(())
}

/// Walks the "interrupts" property of a node; port of thor's dt::walkInterrupts().
/// Does nothing if the node has no interrupts property.
pub fn walk_interrupts<E: From<DtError>>(
    f: &mut impl FnMut(&'static DeviceTreeNode, fdt::Cells<'static>) -> Result<(), E>,
    node: &'static DeviceTreeNode,
) -> Result<(), E> {
    let Some(prop) = node.dt_node.find_property("interrupts") else {
        return Ok(());
    };

    let parent = node.interrupt_parent()?;
    let parent_interrupt_cells = parent.interrupt_cells()?;
    if parent_interrupt_cells == 0 {
        return Err(DtError::NoInterruptCells {
            parent: parent.path().to_string(),
        }
        .into());
    }

    let mut it = prop.access();
    while !it.at_end_of_property() {
        let parent_irq = it
            .into_cells(parent_interrupt_cells)
            .ok_or(DtError::TruncatedProperty { name: "interrupts" })?;
        it.advance(parent_interrupt_cells * size_of::<u32>());

        f(parent, parent_irq)?;
    }

    Ok(())
}

pub fn walk_interrupt_map<E: From<DtError>>(
    f: &mut impl FnMut(
        fdt::Cells<'static>,
        fdt::Cells<'static>,
        &'static DeviceTreeNode,
        fdt::Cells<'static>,
        fdt::Cells<'static>,
    ) -> Result<(), E>,
    node: &'static DeviceTreeNode,
) -> Result<(), E> {
    const TRUNCATED: DtError = DtError::TruncatedProperty {
        name: "interrupt-map",
    };

    let prop = node
        .dt_node
        .find_property("interrupt-map")
        .ok_or(DtError::MissingProperty {
            name: "interrupt-map",
        })?;

    let child_address_cells = node.address_cells()?;
    let child_interrupt_cells = node.interrupt_cells()?;

    let mut it = prop.access();
    while !it.at_end_of_property() {
        let child_address = it.into_cells(child_address_cells).ok_or(TRUNCATED)?;
        it.advance(child_address_cells * size_of::<u32>());
        let child_irq = it.into_cells(child_interrupt_cells).ok_or(TRUNCATED)?;
        it.advance(child_interrupt_cells * size_of::<u32>());

        let parent_phandle = it.read_cells(1).ok_or(TRUNCATED)? as u32;
        it.advance(size_of::<u32>());
        let parent_node =
            get_device_tree_node_by_phandle(parent_phandle).ok_or(DtError::DanglingPhandle {
                phandle: parent_phandle,
            })?;
        // NOTE: This behavior is not documented in the DT specification (the spec says the node
        // should explicitly set #address-cells to 0 if it needs to). This behavior is copied from
        // Linux, and is at least needed to correctly parse interrupt-map of the PCIe node on the
        // RPi4.
        let parent_address_cells =
            parent_node.u32_property("#address-cells")?.unwrap_or(0) as usize;
        let parent_interrupt_cells = parent_node.interrupt_cells()?;

        let parent_address = it.into_cells(parent_address_cells).ok_or(TRUNCATED)?;
        it.advance(parent_address_cells * size_of::<u32>());
        let parent_irq = it.into_cells(parent_interrupt_cells).ok_or(TRUNCATED)?;
        it.advance(parent_interrupt_cells * size_of::<u32>());

        f(
            child_address,
            child_irq,
            parent_node,
            parent_address,
            parent_irq,
        )?;
    }

    Ok(())
}
