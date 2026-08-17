//! Semantic device tree; port of thor's system/dtb/dtb.cpp.

use std::collections::BTreeMap;
use std::sync::{Mutex, OnceLock};

use anyhow::{Context, Result};
use managarm::svrctl::hardware_access_handle;

use crate::acpi::PAGE_MASK;
use crate::dt::fdt;
use crate::dt::irq::IrqController;

// The device tree is only locked for the duration of a single operation, none of which can panic.
const EXPECT_LOCK: &str = "sif: device tree mutex was poisoned";

static PHANDLES: Mutex<BTreeMap<u32, &'static DeviceTreeNode>> = Mutex::new(BTreeMap::new());
static TREE_ROOT: OnceLock<&'static DeviceTreeNode> = OnceLock::new();

pub struct RegRange {
    pub addr_hi: u32,
    pub addr: u64,
    pub size: u64,

    pub addr_hi_valid: bool,
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

    address_cells: usize,
    has_address_cells: bool,
    size_cells: usize,
    interrupt_cells: usize,

    reg: Vec<RegRange>,
    ranges: Vec<AddrTranslateRange>,

    interrupt_controller: bool,

    interrupt_parent_id: u32,
    interrupt_parent: OnceLock<&'static DeviceTreeNode>,

    bus_range: BusRange,

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

impl DeviceTreeNode {
    fn new(
        dt_node: fdt::DeviceTreeNode<'static>,
        parent: Option<&'static DeviceTreeNode>,
    ) -> &'static DeviceTreeNode {
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
            address_cells: 2,
            has_address_cells: false,
            size_cells: 1,
            interrupt_cells: 0,
            reg: Vec::new(),
            ranges: Vec::new(),
            interrupt_controller: false,
            interrupt_parent_id: 0,
            interrupt_parent: OnceLock::new(),
            bus_range: BusRange { from: 0, to: 0xFF },
            associated_irq_controller: OnceLock::new(),
        };

        if let Some(p) = dt_node.find_property("phandle") {
            node.phandle = p.as_u32(0);
        } else if let Some(p) = dt_node.find_property("linux,phandle") {
            println!("sif: warning: node \"{name}\" uses legacy \"linux,phandle\" property!");
            node.phandle = p.as_u32(0);
        }

        for prop in dt_node.properties() {
            match prop.name() {
                "model" => {
                    node.model = fdt::read_string_at(prop.data(), 0);
                }
                "compatible" => {
                    node.compatible = parse_string_list(prop);
                }
                "#address-cells" => {
                    node.address_cells = prop.as_u32(0) as usize;
                    node.has_address_cells = true;
                }
                "#size-cells" => {
                    node.size_cells = prop.as_u32(0) as usize;
                }
                "#interrupt-cells" => {
                    node.interrupt_cells = prop.as_u32(0) as usize;
                }
                "interrupt-parent" => {
                    node.interrupt_parent_id = prop.as_u32(0);
                }
                "interrupt-controller" => {
                    node.interrupt_controller = true;
                }
                "reg" => {
                    let parent = parent.expect("sif: the root node cannot have a reg property");
                    let addr_cells = parent.address_cells;
                    let size_cells = parent.size_cells;

                    let mut j = 0;
                    while j < prop.size() {
                        let mut reg = RegRange {
                            addr_hi: 0,
                            addr: 0,
                            size: 0,
                            addr_hi_valid: false,
                        };

                        if addr_cells != 0 {
                            if addr_cells == 3 {
                                reg.addr_hi = prop.as_prop_array_entry(1, j) as u32;
                                reg.addr_hi_valid = true;
                                reg.addr = prop.as_prop_array_entry(addr_cells - 1, j + 4);
                                j += addr_cells * 4;
                            } else {
                                if j + addr_cells * 4 > prop.size() {
                                    println!(
                                        "sif: warning: node \"{name}\": reg field isn't conforming to #addr-cells"
                                    );
                                    reg.addr = prop.as_prop_array_entry(
                                        (j + addr_cells * 4 - prop.size()) / 4,
                                        0,
                                    );
                                    node.reg.push(reg);
                                    break;
                                }
                                reg.addr = prop.as_prop_array_entry(addr_cells, j);
                                j += addr_cells * 4;
                            }
                        }

                        if size_cells != 0 {
                            if j + size_cells * 4 > prop.size() {
                                println!(
                                    "sif: warning: node \"{name}\": reg field isn't conforming to #size-cells"
                                );
                                reg.size = prop
                                    .as_prop_array_entry((j + size_cells * 4 - prop.size()) / 4, 0);
                                node.reg.push(reg);
                                break;
                            }
                            reg.size = prop.as_prop_array_entry(size_cells, j);
                            j += size_cells * 4;
                        }

                        node.reg.push(reg);
                    }
                }
                "bus-range" => {
                    node.bus_range.from = prop.as_prop_array_entry(1, 0) as u32;
                    node.bus_range.to = prop.as_prop_array_entry(1, 4) as u32;
                }
                _ => {}
            }
        }

        // Iterate again to parse things that depend on previously parsed properties.
        for prop in dt_node.properties() {
            if prop.name() == "ranges" {
                let parent_addr_cells = parent
                    .expect("sif: the root node cannot have a ranges property")
                    .address_cells;
                let child_addr_cells = node.address_cells;
                let size_cells = node.size_cells;

                let mut j = 0;
                while j < prop.size() {
                    let mut reg = AddrTranslateRange {
                        child_addr_hi: 0,
                        child_addr: 0,
                        parent_addr: 0,
                        size: 0,
                        child_addr_hi_valid: false,
                    };
                    // PCI(e) buses have a 3 cell long child addresses.
                    if child_addr_cells == 3 {
                        reg.child_addr_hi = prop.as_prop_array_entry(1, j) as u32;
                        j += 4;
                        reg.child_addr = prop.as_prop_array_entry(2, j);
                        j += 8;
                        reg.child_addr_hi_valid = true;
                    } else {
                        assert!(child_addr_cells < 3);
                        reg.child_addr = prop.as_prop_array_entry(child_addr_cells, j);
                        j += child_addr_cells * 4;
                    }

                    assert!(parent_addr_cells < 3);
                    reg.parent_addr = prop.as_prop_array_entry(parent_addr_cells, j);
                    j += parent_addr_cells * 4;

                    reg.size = prop.as_prop_array_entry(size_cells, j);
                    j += size_cells * 4;

                    node.ranges.push(reg);
                }
            }
        }

        {
            // Inherit the interrupt parent from the parent if we don't have one.
            let mut p = parent;
            while node.interrupt_parent_id == 0 {
                let Some(parent) = p else {
                    break;
                };
                if parent.is_interrupt_controller() {
                    assert!(parent.phandle != 0);
                    node.interrupt_parent_id = parent.phandle;
                    continue;
                }

                node.interrupt_parent_id = parent.interrupt_parent_id;
                p = parent.parent;
            }
        }

        // Unlike thor, addresses are translated at construction time; this is equivalent
        // since parents are always fully initialized before their children.
        if let Some(parent) = parent
            && !parent.ranges.is_empty()
        {
            for r in &mut node.reg {
                r.addr = parent.translate_address(r.addr);
            }

            for r in &mut node.ranges {
                r.parent_addr = parent.translate_address(r.parent_addr);
            }
        }

        let node: &'static DeviceTreeNode = Box::leak(Box::new(node));
        if node.phandle != 0 {
            PHANDLES
                .lock()
                .expect(EXPECT_LOCK)
                .insert(node.phandle, node);
        }
        node
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

    fn finalize_init(&'static self) {
        if self.interrupt_parent_id != 0 {
            match get_device_tree_node_by_phandle(self.interrupt_parent_id) {
                Some(ip) => {
                    let _ = self.interrupt_parent.set(ip);
                }
                None => panic!(
                    "sif: node \"{}\" has an interrupt parent id {} but no such node exists",
                    self.name, self.interrupt_parent_id
                ),
            }
        }

        // Recurse into children.
        for child in self.children.lock().expect(EXPECT_LOCK).iter() {
            child.finalize_init();
        }
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

    pub fn bus_range(&self) -> &BusRange {
        &self.bus_range
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

    pub fn translate_address(&self, addr: u64) -> u64 {
        // We only handle simple bus address translation.
        if !self.is_compatible(&["simple-bus"]) {
            return addr;
        }

        // This node has no translation table.
        if self.ranges.is_empty() {
            return addr;
        }

        for tr in &self.ranges {
            if addr >= tr.child_addr && addr < tr.child_addr + tr.size {
                return tr.parent_addr + (addr - tr.child_addr);
            }
        }

        panic!(
            "sif: address {addr:#x} doesn't fall into any of \"{}\"'s memory ranges",
            self.path
        );
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

    let root = DeviceTreeNode::new(tree.root_node(), None);
    assert!(
        TREE_ROOT.set(root).is_ok(),
        "sif: device tree was already initialized"
    );

    println!("sif: Booting on \"{}\"", root.model);

    struct Walker {
        curr: Option<&'static DeviceTreeNode>,
    }

    impl fdt::DeviceTreeWalker<'static> for Walker {
        fn push(&mut self, dt_node: fdt::DeviceTreeNode<'static>) {
            let curr = self.curr.expect("sif: device tree walker escaped the root");
            let node = DeviceTreeNode::new(dt_node, Some(curr));
            curr.attach_child(node);
            self.curr = Some(node);
        }

        fn pop(&mut self) {
            self.curr = self
                .curr
                .expect("sif: device tree walker escaped the root")
                .parent;
        }
    }

    let mut walker = Walker { curr: Some(root) };
    tree.root_node().walk_children(&mut walker);

    // Initialize the interrupt parents. This can't be done above because the interrupt
    // parent may not have been discovered yet.
    root.finalize_init();

    Ok(())
}

pub fn walk_interrupt_map(
    f: &mut impl FnMut(
        fdt::Cells<'static>,
        fdt::Cells<'static>,
        &'static DeviceTreeNode,
        fdt::Cells<'static>,
        fdt::Cells<'static>,
    ),
    node: &'static DeviceTreeNode,
) -> bool {
    let Some(prop) = node.dt_node.find_property("interrupt-map") else {
        println!("sif: {} has no interrupt-map", node.path());
        return false;
    };

    let child_address_cells = node.address_cells;
    let child_interrupt_cells = node.interrupt_cells;

    let mut it = prop.access();
    while !it.at_end_of_property() {
        let Some(child_address) = it.into_cells(child_address_cells) else {
            println!(
                "sif: {}: failed to read child address from interrupt-map",
                node.path()
            );
            return false;
        };
        it.advance(child_address_cells * size_of::<u32>());
        let Some(child_irq) = it.into_cells(child_interrupt_cells) else {
            println!(
                "sif: {}: failed to read child IRQ from interrupt-map",
                node.path()
            );
            return false;
        };
        it.advance(child_interrupt_cells * size_of::<u32>());

        let Some(parent_phandle) = it.read_cells(1) else {
            println!(
                "sif: {}: failed to read phandle from interrupt-map",
                node.path()
            );
            return false;
        };
        it.advance(size_of::<u32>());
        let Some(parent_node) = get_device_tree_node_by_phandle(parent_phandle as u32) else {
            println!(
                "sif: {}: no DT node with phandle {parent_phandle}",
                node.path()
            );
            return false;
        };
        // NOTE: This behavior is not documented in the DT specification (the spec says the node
        // should explicitly set #address-cells to 0 if it needs to). This behavior is copied from
        // Linux, and is at least needed to correctly parse interrupt-map of the PCIe node on the
        // RPi4.
        let parent_address_cells = if parent_node.has_address_cells {
            parent_node.address_cells
        } else {
            0
        };
        let parent_interrupt_cells = parent_node.interrupt_cells;

        let Some(parent_address) = it.into_cells(parent_address_cells) else {
            println!(
                "sif: {}: failed to read parent address from interrupt-map",
                node.path()
            );
            return false;
        };
        it.advance(parent_address_cells * size_of::<u32>());
        let Some(parent_irq) = it.into_cells(parent_interrupt_cells) else {
            println!(
                "sif: {}: failed to read parent IRQ from interrupt-map",
                node.path()
            );
            return false;
        };
        it.advance(parent_interrupt_cells * size_of::<u32>());

        f(
            child_address,
            child_irq,
            parent_node,
            parent_address,
            parent_irq,
        );
    }

    true
}
