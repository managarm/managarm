//! Drives the IOMMUs that the kernel discovered from the DMAR table.
//!
//! The kernel owns the hardware; sif supplies the one thing that it cannot compute without a
//! PCI tree: which requester ID belongs to which domain, and how domains are cut.

use managarm::svrctl::hardware_access_handle;
use std::collections::HashMap;
use std::sync::atomic::Ordering;

use hel::{DmaDeviceId, DmaReservedRegion, IommuKind};

use crate::acpi::PAGE_SIZE;
use crate::acpi::dmar::{self, DeviceScope};

use super::discover::all_root_buses;
use super::{
    EXPECT_LOCK, PCIE_TYPE_DOWNSTREAM_PORT, PCIE_TYPE_PCIE_TO_PCI_BRIDGE, PCIE_TYPE_ROOT_PORT,
    PCIE_TYPE_UPSTREAM_PORT, PciBridge, PciBus, PciDevice, PciEntity, leak,
};

/// One IOMMU, i.e., one DMA remapping hardware unit.
pub struct IommuUnit {
    handle: hel::Handle,
    segment: u16,
}

/// One IOMMU domain, i.e., a DMA space that a set of requesters shares.
pub struct DmaDomain {
    handle: hel::Handle,
}

impl DmaDomain {
    pub fn handle(&self) -> &hel::Handle {
        &self.handle
    }
}

fn entity_id(entity: &PciEntity) -> DmaDeviceId {
    DmaDeviceId {
        segment: entity.seg,
        bus: entity.bus,
        slot: entity.slot,
        function: entity.function,
    }
}

/// Binds a requester ID to a DMA space, or to the passthrough domain if `domain` is `None`.
async fn bind_id(
    unit: &'static IommuUnit,
    domain: Option<&'static DmaDomain>,
    id: DmaDeviceId,
) -> bool {
    let result =
        hel::submission::bind_dma_device(&unit.handle, domain.map(DmaDomain::handle), id).await;
    if let Err(err) = result {
        println!(
            "sif: Failed to bind {:04x}:{:02x}:{:02x}.{} to its IOMMU domain: {err}",
            id.segment, id.bus, id.slot, id.function
        );
        return false;
    }
    true
}

/// Binds every requester ID that an entity DMAs as to a DMA space, or to the passthrough
/// domain if `domain` is `None`.
pub async fn bind_device(
    unit: &'static IommuUnit,
    domain: Option<&'static DmaDomain>,
    entity: &'static PciEntity,
) -> bool {
    let mut bound = true;
    for id in alias_ids(entity) {
        bound &= bind_id(unit, domain, id).await;
    }
    bound
}

/// Returns the DMA space that the driver of an entity maps into, and whether an IOMMU
/// translates it.
///
/// A driver may ask more than once and has to be handed the same space every time. An entity
/// that no IOMMU translates gets a space of its own that does not translate, so that drivers
/// cannot reach each other's mappings through a shared one.
pub fn dma_space(entity: &'static PciEntity) -> hel::Result<(bool, hel::Handle)> {
    if let Some(domain) = entity.dma_domain.get() {
        return Ok((true, domain.handle().clone_handle()?));
    }

    let space = match entity.noop_dma_space.get() {
        Some(space) => space,
        None => {
            // Two callers racing here each create a space; the loser's is closed right away.
            let created = hel::create_dma_space(None, &[])?;
            entity.noop_dma_space.get_or_init(|| created)
        }
    };
    Ok((false, space.clone_handle()?))
}

/// The requester ID that a bridge tags the DMA requests that it forwards upstream with.
enum BridgeAlias {
    /// PCI Express root ports and switch ports forward the requester ID unchanged.
    None,
    /// Conventional PCI bridges issue the requests as the bridge itself (VT-d 3.12.2).
    OwnId,
    /// PCI Express-to-PCI/PCI-X bridges may take ownership of a request and re-tag it with
    /// the secondary bus number and a device and function number of zero (VT-d 3.12.1).
    SecondaryBus,
}

/// Returns the requester IDs that the DMA requests of an entity can arrive with: its own,
/// plus the one that every bridge above it re-tags forwarded requests with.
///
/// VT-d 3.12.1 requires that all of them are programmed identically.
fn alias_ids(entity: &'static PciEntity) -> Vec<DmaDeviceId> {
    let mut ids = vec![entity_id(entity)];

    let mut bus = entity.parent_bus;
    while let Some(bridge) = bus.associated_bridge {
        match bridge_alias(bridge) {
            BridgeAlias::None => (),
            BridgeAlias::OwnId => ids.push(entity_id(&bridge.entity)),
            // The bridge re-tags with its secondary bus number, i.e. with the bus that we
            // are currently walking up from.
            BridgeAlias::SecondaryBus => ids.push(DmaDeviceId {
                segment: bus.seg_id,
                bus: bus.bus_id,
                slot: 0,
                function: 0,
            }),
        }
        bus = bridge.entity.parent_bus;
    }

    ids
}

fn bridge_alias(bridge: &PciBridge) -> BridgeAlias {
    if !bridge.entity.is_pcie.load(Ordering::Relaxed) {
        return BridgeAlias::OwnId;
    }
    match bridge.entity.pcie_port_type.load(Ordering::Relaxed) {
        PCIE_TYPE_ROOT_PORT | PCIE_TYPE_UPSTREAM_PORT | PCIE_TYPE_DOWNSTREAM_PORT => {
            BridgeAlias::None
        }
        PCIE_TYPE_PCIE_TO_PCI_BRIDGE => BridgeAlias::SecondaryBus,
        // Anything else that bridges to PCI is assumed to take ownership: grouping a
        // requester too coarsely only costs isolation, grouping it too finely breaks DMA.
        _ => BridgeAlias::OwnId,
    }
}

/// Returns the IOMMU that translates the DMA requests of an entity.
///
/// A DRHD that covers a whole segment only claims the entities on the root buses; everything
/// behind a bridge inherits the unit of that bridge.
pub fn find_iommu(entity: &'static PciEntity) -> Option<&'static IommuUnit> {
    if let Some(unit) = entity.associated_iommu.get() {
        return Some(unit);
    }

    let mut bridge = entity.parent_bus.associated_bridge;
    while let Some(candidate) = bridge {
        if let Some(unit) = candidate.entity.associated_iommu.get() {
            return Some(unit);
        }
        bridge = candidate.entity.parent_bus.associated_bridge;
    }
    None
}

enum ScopeTarget {
    Device(&'static PciDevice),
    Bridge(&'static PciBridge),
}

impl ScopeTarget {
    fn entity(&self) -> &'static PciEntity {
        match self {
            ScopeTarget::Device(device) => &device.entity,
            ScopeTarget::Bridge(bridge) => &bridge.entity,
        }
    }
}

fn find_bridge(bus: &'static PciBus, slot: u8, function: u8) -> Option<&'static PciBridge> {
    bus.child_bridges
        .lock()
        .expect(EXPECT_LOCK)
        .iter()
        .find(|bridge| bridge.entity.slot == slot && bridge.entity.function == function)
        .copied()
}

fn find_device(bus: &'static PciBus, slot: u8, function: u8) -> Option<&'static PciDevice> {
    bus.child_devices
        .lock()
        .expect(EXPECT_LOCK)
        .iter()
        .find(|device| device.entity.slot == slot && device.entity.function == function)
        .copied()
}

/// Resolves a DMAR device scope against our PCI tree.
fn resolve_scope(scope: &DeviceScope, segment: u16) -> Option<ScopeTarget> {
    let mut bus = all_root_buses()
        .into_iter()
        .find(|bus| bus.seg_id == segment && bus.bus_id == scope.start_bus)?;

    // Every entry but the last one names a bridge that the path descends through.
    let (&(slot, function), path) = scope.path.split_last()?;
    for &(bridge_slot, bridge_function) in path {
        let bridge = find_bridge(bus, bridge_slot, bridge_function)?;
        let downstream = *bridge.associated_bus.get()?;
        if downstream.bus_id == bus.bus_id {
            return None;
        }
        bus = downstream;
    }

    match scope.type_ {
        dmar::SCOPE_PCI_ENDPOINT => find_device(bus, slot, function).map(ScopeTarget::Device),
        dmar::SCOPE_PCI_BRIDGE => find_bridge(bus, slot, function).map(ScopeTarget::Bridge),
        _ => None,
    }
}

/// Associates the entities that a DRHD covers with its unit.
fn attach_scopes(unit: &'static IommuUnit, drhd: &dmar::Drhd) {
    if drhd.include_pci_all {
        // Bridges on a root bus are treated like PCI sub-hierarchy scopes, which is what makes
        // find_iommu() resolve everything behind them without recursing here.
        for bus in all_root_buses()
            .into_iter()
            .filter(|bus| bus.seg_id == unit.segment)
        {
            for device in bus.child_devices.lock().expect(EXPECT_LOCK).iter() {
                let _ = device.entity.associated_iommu.set(unit);
            }
            for bridge in bus.child_bridges.lock().expect(EXPECT_LOCK).iter() {
                let _ = bridge.entity.associated_iommu.set(unit);
            }
        }
        return;
    }

    for scope in &drhd.scopes {
        let Some(target) = resolve_scope(scope, drhd.segment) else {
            // IOAPIC and HPET scopes resolve to no PCI entity, as do paths that firmware
            // describes but that we did not enumerate.
            continue;
        };
        let _ = target.entity().associated_iommu.set(unit);
    }
}

/// A domain that has been cut but whose DMA space has not been created yet: the reserved
/// regions have to be known before the space becomes observable.
struct PendingDomain {
    unit: &'static IommuUnit,
    members: Vec<&'static PciEntity>,
    regions: Vec<DmaReservedRegion>,
}

/// Requester ID, used to look a pending domain up by the entity that it was cut for.
type EntityKey = (u16, u8, u8, u8);

fn entity_key(entity: &PciEntity) -> EntityKey {
    (entity.seg, entity.bus, entity.slot, entity.function)
}

struct DomainPolicy {
    domains: Vec<PendingDomain>,
    of_entity: HashMap<EntityKey, usize>,
}

impl DomainPolicy {
    fn new_domain(&mut self, entity: &'static PciEntity) -> Option<usize> {
        let unit = find_iommu(entity)?;
        self.domains.push(PendingDomain {
            unit,
            members: Vec::new(),
            regions: Vec::new(),
        });
        Some(self.domains.len() - 1)
    }

    /// Returns the domain that an entity is placed in. A candidate domain that a different
    /// IOMMU translates cannot hold the entity, hence a new domain is cut in that case.
    fn domain_for(&mut self, candidate: Option<usize>, entity: &'static PciEntity) -> Option<usize> {
        let unit = find_iommu(entity)?;
        match candidate {
            Some(domain) if std::ptr::eq(self.domains[domain].unit, unit) => Some(domain),
            _ => self.new_domain(entity),
        }
    }

    fn add_member(&mut self, domain: usize, entity: &'static PciEntity) {
        self.domains[domain].members.push(entity);
        self.of_entity.insert(entity_key(entity), domain);
    }

    /// Returns the domain that the members of a bus share, cutting it on first use.
    ///
    /// Cutting lazily matters because a hardware domain id is a scarce resource: VT-d units
    /// report as few as 16 of them, and the kernel never reuses one.
    fn common_domain(&mut self, bus: &'static PciBus, cache: &mut Option<usize>) -> Option<usize> {
        if cache.is_none() {
            *cache = self.new_domain(&bus.associated_bridge?.entity);
        }
        *cache
    }

    /// Places one entity of a bus in a domain and returns it.
    fn place_entity(
        &mut self,
        bus: &'static PciBus,
        entity: &'static PciEntity,
        collapsed: bool,
        split_device_domains: bool,
        common: &mut Option<usize>,
        multifunction_domains: &mut [Option<usize>; 32],
    ) -> Option<usize> {
        // The multifunction bit is only defined in function 0 of a slot.
        let multifunction = bus.header_type(entity.slot, 0) & 0x80 != 0;

        let candidate = if collapsed {
            *common
        } else if multifunction {
            multifunction_domains[usize::from(entity.slot)]
        } else if split_device_domains {
            None
        } else {
            self.common_domain(bus, common)
        };

        let domain = self.domain_for(candidate, entity);
        // Without ACS, the functions of a multifunction device can DMA to each other
        // directly, hence isolating them from each other buys nothing.
        if multifunction && !collapsed {
            multifunction_domains[usize::from(entity.slot)] = domain;
        }

        if let Some(domain) = domain {
            self.add_member(domain, entity);
        }
        domain
    }

    /// Cuts the domains of one bus and, recursively, of everything behind it. Returns the
    /// domain that the members of the bus share, if one was cut.
    ///
    /// Devices on a root bus and the functions of a multifunction device get their own domain;
    /// everything else shares the domain of the bridge that it sits behind. `inherited` is the
    /// domain of a bridge that cannot be separated from its sub-hierarchy.
    fn walk_bus(
        &mut self,
        bus: &'static PciBus,
        inherited: Option<usize>,
        is_root_bus: bool,
    ) -> Option<usize> {
        // Intel VT-d requires that root complex integrated endpoints can be split into
        // independent domains. TODO: split behind bridges that support ACS as well.
        let split_device_domains = is_root_bus;
        let collapsed = inherited.is_some();

        let mut common_domain = inherited;
        let mut multifunction_domains: [Option<usize>; 32] = [None; 32];

        let devices = bus.child_devices.lock().expect(EXPECT_LOCK).clone();
        for device in devices {
            self.place_entity(
                bus,
                &device.entity,
                collapsed,
                split_device_domains,
                &mut common_domain,
                &mut multifunction_domains,
            );
        }

        let bridges = bus.child_bridges.lock().expect(EXPECT_LOCK).clone();
        for bridge in bridges {
            let domain = self.place_entity(
                bus,
                &bridge.entity,
                collapsed,
                split_device_domains,
                &mut common_domain,
                &mut multifunction_domains,
            );

            let Some(&downstream) = bridge.associated_bus.get() else {
                continue;
            };

            // A bridge that issues forwarded requests as itself cannot be told apart from
            // the devices behind it, hence it shares their domain (VT-d 3.12.2). One that
            // re-tags them with its secondary bus number can: that requester ID belongs to
            // no entity of ours, and alias_ids() binds it alongside each device below.
            let inherited = match bridge_alias(bridge) {
                BridgeAlias::OwnId => domain,
                BridgeAlias::None | BridgeAlias::SecondaryBus => None,
            };
            self.walk_bus(downstream, inherited, false);
        }

        common_domain
    }

    /// Collects the domains that a device scope applies to.
    fn domains_of_scope(&self, target: &ScopeTarget, out: &mut Vec<usize>) {
        if let Some(&domain) = self.of_entity.get(&entity_key(target.entity()))
            && !out.contains(&domain)
        {
            out.push(domain);
        }

        // A PCI sub-hierarchy scope names the bridge but covers every device below it as
        // well (VT-d 8.3.1), and those do not have to share the domain of the bridge.
        if let ScopeTarget::Bridge(bridge) = target
            && let Some(&downstream) = bridge.associated_bus.get()
        {
            self.domains_of_bus(downstream, out);
        }
    }

    fn domains_of_bus(&self, bus: &'static PciBus, out: &mut Vec<usize>) {
        let devices = bus.child_devices.lock().expect(EXPECT_LOCK).clone();
        let bridges = bus.child_bridges.lock().expect(EXPECT_LOCK).clone();

        for entity in devices
            .iter()
            .map(|device| &device.entity)
            .chain(bridges.iter().map(|bridge| &bridge.entity))
        {
            if let Some(&domain) = self.of_entity.get(&entity_key(entity))
                && !out.contains(&domain)
            {
                out.push(domain);
            }
        }

        for bridge in bridges {
            if let Some(&downstream) = bridge.associated_bus.get() {
                self.domains_of_bus(downstream, out);
            }
        }
    }

    /// Adds the reserved region of an RMRR to every domain that it applies to.
    fn add_rmrr(&mut self, rmrr: &dmar::Rmrr) {
        // Firmware is required to report a 4KB-aligned range (VT-d 8.4), but a mapping
        // has to be aligned whether or not it does.
        let Some(end) = rmrr.limit.checked_add(1) else {
            println!("sif: Ignoring RMRR with an out-of-range limit");
            return;
        };
        if end <= rmrr.base {
            println!("sif: Ignoring RMRR with a limit below its base");
            return;
        }
        let base = rmrr.base & !(PAGE_SIZE as u64 - 1);
        let size = (end - base).next_multiple_of(PAGE_SIZE as u64);

        let mut domains = Vec::new();
        for scope in &rmrr.scopes {
            let Some(target) = resolve_scope(scope, rmrr.segment) else {
                continue;
            };
            self.domains_of_scope(&target, &mut domains);
        }

        for domain in domains {
            // An RMRR may legally refer to multiple devices that share a domain.
            let regions = &mut self.domains[domain].regions;
            if regions.iter().any(|region| region.base == base) {
                continue;
            }

            println!("sif: Punching through RMRR at {base:#012x} (size {size:#x}) for domain {domain}");
            regions.push(DmaReservedRegion {
                base,
                size,
                readable: true,
                writable: true,
            });
        }
    }
}

/// Discovers the IOMMUs, cuts the domains, binds every enumerated requester and finally
/// activates translation.
///
/// This has to complete before any device is published: after the activation an unbound
/// requester is blocked, and before it a translated DMA space would hand a driver addresses
/// that nothing translates.
pub async fn configure() {
    let Some(dmar) = dmar::parse() else {
        return;
    };

    let mut units = Vec::new();
    for drhd in &dmar.drhds {
        // A failure means that thor does not drive the unit; leave its devices untranslated.
        let handle = match hel::access_iommu(
            hardware_access_handle(),
            IommuKind::IntelVtd,
            drhd.register_base,
        ) {
            Ok(handle) => handle,
            Err(err) => {
                println!(
                    "sif: Cannot access the IOMMU at {:#x}: {err}",
                    drhd.register_base
                );
                continue;
            }
        };

        let unit: &'static IommuUnit = leak(IommuUnit {
            handle,
            segment: drhd.segment,
        });
        attach_scopes(unit, drhd);
        units.push(unit);
    }

    if units.is_empty() {
        return;
    }

    let mut policy = DomainPolicy {
        domains: Vec::new(),
        of_entity: HashMap::new(),
    };
    for bus in all_root_buses() {
        policy.walk_bus(bus, None, true);
    }
    for rmrr in &dmar.rmrrs {
        policy.add_rmrr(rmrr);
    }

    for pending in &policy.domains {
        let handle = match hel::create_dma_space(Some(&pending.unit.handle), &pending.regions) {
            Ok(handle) => handle,
            Err(err) => {
                println!("sif: Failed to create an IOMMU domain: {err}");
                continue;
            }
        };

        let domain: &'static DmaDomain = leak(DmaDomain { handle });
        for entity in &pending.members {
            let _ = entity.dma_domain.set(domain);
        }
    }

    // Requesters that we did not cut a domain for are bound passthrough so that they keep
    // working once translation is on.
    for bus in all_root_buses() {
        bind_bus(bus).await;
    }

    for unit in units {
        if let Err(err) = hel::submission::activate_iommu(&unit.handle).await {
            println!("sif: Failed to activate an IOMMU: {err}");
        }
    }

    println!("sif: Activated {} IOMMU domains", policy.domains.len());
}

async fn bind_entity(entity: &'static PciEntity) {
    let Some(unit) = find_iommu(entity) else {
        return;
    };
    bind_device(unit, entity.dma_domain.get().copied(), entity).await;
}

async fn bind_bus(bus: &'static PciBus) {
    let devices = bus.child_devices.lock().expect(EXPECT_LOCK).clone();
    for device in devices {
        bind_entity(&device.entity).await;
    }
    let bridges = bus.child_bridges.lock().expect(EXPECT_LOCK).clone();
    for bridge in bridges {
        bind_entity(&bridge.entity).await;
        if let Some(&downstream) = bridge.associated_bus.get() {
            Box::pin(bind_bus(downstream)).await;
        }
    }
}
