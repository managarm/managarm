//! dmalog: provides kernel I/O channels backed by QEMU's dmalog PCI devices.
//!
//! thor binds dmalog itself when it enumerates PCI, but under sif the bus belongs to
//! userspace, so this server drives the device and hands each stream back to the kernel
//! through kerncfg's ProvideIoChannel as a UserIoChannel.

use std::ffi::c_void;
use std::ptr::{self, NonNull};
use std::rc::Rc;

use anyhow::{Context, Result, ensure};
use arch::{BitValue, IoMemSpace, bit_register, scalar_register};
use event_listener::Event;
use hel::{AllocFlags, Handle, Mapping, MappingFlags};
use managarm::mbus::{self, EventType, Item};
use managarm::shm;
use managarm::{hw, kerncfg, posix};

const PAGE_SIZE: usize = 4096;

// Register layout of BAR 0; mirrors kernel/thor/system/pci/dmalog.cpp.
scalar_register!(OutRegister @ 0x0: u64);
scalar_register!(InRegister @ 0x8: u64);
bit_register! {
    IsrRegister @ 0x10: u32 {
        OUT_STATUS @ 0, 1: bool;
        IN_STATUS @ 1, 1: bool;
    }
}
const TAG_OFFSET: usize = 0x40;
const TAG_SIZE: usize = 64;

// The control page holds the output descriptor at 0 and the input descriptor at 2048.
const OUT_DESCRIPTOR_OFFSET: usize = 0;
const IN_DESCRIPTOR_OFFSET: usize = 2048;
const MAX_BUFFERS: usize = (2048 - 32) / 16;
// Largest transfer whose sglist fits into a descriptor, even if it does not start page-aligned.
const MAX_TRANSFER: usize = (MAX_BUFFERS - 1) * PAGE_SIZE;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Sglist {
    ptr: u64,
    length: u64,
}

#[repr(C)]
struct Descriptor {
    status: u64,
    actual_length: u64,
    flags: u64,
    num_buffers: u64,
    buffers: [Sglist; MAX_BUFFERS],
}

/// The data area of a kernel ring with the physical address of each page so that the
/// device can DMA straight from / into the kernel's memory.
struct Ring {
    size: usize,
    pages: Vec<u64>,
}

impl Ring {
    fn new(data: NonNull<u8>, size: usize) -> Result<Ring> {
        let pages = (0..size / PAGE_SIZE)
            .map(|i| {
                let page = unsafe { data.as_ptr().add(i * PAGE_SIZE) };
                // Touch the page so that it is populated before its physical address is queried.
                unsafe { ptr::read_volatile(page) };
                hel::pointer_physical(page as *const c_void).map(|physical| physical as u64)
            })
            .collect::<hel::Result<Vec<u64>>>()?;
        Ok(Ring { size, pages })
    }

    // Splits the ring range [position, position + length) into per-page DMA buffers.
    fn sglist(&self, position: u64, length: usize) -> Vec<Sglist> {
        let mut buffers = Vec::new();
        let mut progress = 0;
        while progress < length {
            let offset = (position as usize + progress) & (self.size - 1);
            let misalign = offset & (PAGE_SIZE - 1);
            let chunk = (length - progress).min(PAGE_SIZE - misalign);
            buffers.push(Sglist {
                ptr: self.pages[offset / PAGE_SIZE] + misalign as u64,
                length: chunk as u64,
            });
            progress += chunk;
        }
        buffers
    }
}

struct Device {
    tag: String,
    mmio: IoMemSpace,
    _bar: Mapping<u8>,
    ctrl: NonNull<u8>,
    _ctrl_mapping: Mapping<u8>,
    ctrl_physical: u64,
    out_ring: Ring,
    in_ring: Ring,
    // Notified by the IRQ task whenever a transfer completes.
    completion: Event,
}

impl Device {
    fn descriptor(&self, offset: usize) -> *mut Descriptor {
        unsafe { self.ctrl.as_ptr().add(offset) }.cast()
    }

    fn post_descriptor(&self, offset: usize, ring: &Ring, position: u64, length: usize) {
        let buffers = ring.sglist(position, length);
        assert!(buffers.len() <= MAX_BUFFERS);
        let mut descriptor = Descriptor {
            status: 0,
            actual_length: 0,
            flags: 1, // Raise an interrupt on completion.
            num_buffers: buffers.len() as u64,
            buffers: [Sglist::default(); MAX_BUFFERS],
        };
        descriptor.buffers[..buffers.len()].copy_from_slice(&buffers);
        // The device reads the descriptor via DMA, hence write it out before the doorbell.
        unsafe { ptr::write_volatile(self.descriptor(offset), descriptor) };
    }

    // Waits until the device writes a status back to the descriptor; returns actual_length.
    async fn await_completion(&self, offset: usize) -> u64 {
        let descriptor = self.descriptor(offset);
        while unsafe { ptr::read_volatile(ptr::addr_of!((*descriptor).status)) } == 0 {
            self.completion.listen().await;
        }
        unsafe { ptr::read_volatile(ptr::addr_of!((*descriptor).actual_length)) }
    }

    /// Writes the output ring range [from, to) to the host.
    async fn transmit(&self, from: u64, to: u64) -> Result<()> {
        self.post_descriptor(
            OUT_DESCRIPTOR_OFFSET,
            &self.out_ring,
            from,
            (to - from) as usize,
        );
        unsafe {
            self.mmio.store(
                OutRegister,
                self.ctrl_physical + OUT_DESCRIPTOR_OFFSET as u64,
            )
        };
        let actual_length = self.await_completion(OUT_DESCRIPTOR_OFFSET).await;
        ensure!(
            actual_length == to - from,
            "dmalog: short transmit on {}",
            self.tag
        );
        Ok(())
    }

    /// Fills the input ring starting at `position` with up to `length` bytes from the host.
    /// Returns the number of bytes received.
    async fn receive(&self, position: u64, length: usize) -> Result<u64> {
        self.post_descriptor(IN_DESCRIPTOR_OFFSET, &self.in_ring, position, length);
        unsafe {
            self.mmio
                .store(InRegister, self.ctrl_physical + IN_DESCRIPTOR_OFFSET as u64)
        };
        let actual_length = self.await_completion(IN_DESCRIPTOR_OFFSET).await;
        ensure!(
            actual_length != 0 && actual_length <= length as u64,
            "dmalog: bad receive length on {}",
            self.tag
        );
        Ok(actual_length)
    }
}

async fn handle_irqs(device: Rc<Device>, irq: Handle) -> Result<()> {
    let mut sequence = 0;
    loop {
        sequence = hel::await_event(&irq, sequence).await?;

        let isr = unsafe { device.mmio.load(IsrRegister) };
        let out_irq = isr.get(IsrRegister::OUT_STATUS);
        let in_irq = isr.get(IsrRegister::IN_STATUS);
        if !out_irq && !in_irq {
            hel::acknowledge_irq(&irq, hel_sys::kHelAckNack, sequence)?;
            continue;
        }

        // Writing the status bits back clears them.
        unsafe {
            device.mmio.store(
                IsrRegister,
                BitValue::zero()
                    .with(IsrRegister::OUT_STATUS, out_irq)
                    .with(IsrRegister::IN_STATUS, in_irq),
            )
        };
        hel::acknowledge_irq(&irq, hel_sys::kHelAckAcknowledge, sequence)?;

        device.completion.notify(usize::MAX);
    }
}

/// Writes the kernel's output to the host until the kernel closes the channel.
async fn serve_output(device: Rc<Device>, mut output: shm::Consumer) -> Result<()> {
    loop {
        match output.wait_for_data(1).await {
            Ok(()) => {}
            Err(shm::Error::PeerClosed) => return Ok(()),
            Err(e) => return Err(e.into()),
        }
        let position = output.position();
        let length = output.available_size().min(MAX_TRANSFER);
        device.transmit(position, position + length as u64).await?;
        output.consume(length);
    }
}

/// Passes input from the host to the kernel until the kernel closes the channel.
async fn serve_input(device: Rc<Device>, mut input: shm::Producer) -> Result<()> {
    loop {
        match input.wait_for_space(1).await {
            Ok(()) => {}
            Err(shm::Error::PeerClosed) => return Ok(()),
            Err(e) => return Err(e.into()),
        }
        let length = input.free_size().min(MAX_TRANSFER);
        let received = device.receive(input.head(), length).await?;
        input.produce(received as usize);
    }
}

fn read_tag(mmio: &IoMemSpace) -> String {
    let mut tag = Vec::new();
    for i in 0..TAG_SIZE {
        let c = unsafe { mmio.scalar_load::<u8>(TAG_OFFSET + i) };
        if c == 0 {
            break;
        }
        tag.push(c);
    }
    String::from_utf8_lossy(&tag).into_owned()
}

async fn bind_device(entity: mbus::Entity) -> Result<()> {
    let hw_device = hw::Device::new(entity.get_remote_lane().await?);
    let info = hw_device.get_pci_info().await?;
    let bar_info = info.bar_info()[0];
    ensure!(
        bar_info.io_type() == hw::pci::IoType::Memory,
        "dmalog: BAR 0 is not a memory BAR"
    );

    let bar = hw_device.access_bar(0).await?;
    let bar_mapping = unsafe {
        Mapping::<u8>::new(
            &bar,
            None,
            bar_info.offset() as usize,
            bar_info.length(),
            MappingFlags::READ | MappingFlags::WRITE,
        )
    }?;
    let mmio = unsafe {
        IoMemSpace::new(
            bar_mapping.as_ptr().context("BAR is not mapped")?.as_ptr(),
            bar_info.length(),
        )
    };
    let tag = read_tag(&mmio);
    println!("dmalog: Found device with tag {tag}");

    hw_device.enable_busmaster().await?;
    let msi = if info.num_msis() > 0 {
        hw_device.install_msi(0).await.ok()
    } else {
        None
    };
    let irq = match msi {
        Some(irq) => {
            hw_device.enable_msi().await?;
            irq
        }
        None => {
            let irq = hw_device.access_irq(0).await?;
            hw_device.enable_bus_irq().await?;
            irq
        }
    };

    // The device DMA-reads its descriptors, so they live in a physically contiguous page.
    let ctrl_memory = hel::allocate_memory(
        posix::hierarchy_handle(),
        PAGE_SIZE,
        AllocFlags::CONTINUOUS,
        Some(32),
    )?;
    let ctrl_mapping = unsafe {
        Mapping::<u8>::new(
            &ctrl_memory,
            None,
            0,
            PAGE_SIZE,
            MappingFlags::READ | MappingFlags::WRITE,
        )
    }?;
    let ctrl = unsafe { ctrl_mapping.as_ptr() }.context("control page is not mapped")?;
    unsafe { ptr::write_bytes(ctrl.as_ptr(), 0, PAGE_SIZE) };
    let ctrl_physical = hel::pointer_physical(ctrl.as_ptr() as *const c_void)? as u64;

    let channel = kerncfg::provide_io_channel(&tag, &format!("dmalog-{tag}"), true).await?;
    let output = channel.output;
    let input = channel.input.context("no input ring")?;
    let out_ring = Ring::new(output.data_ptr(), output.size())?;
    let in_ring = Ring::new(input.data_ptr(), input.size())?;

    let device = Rc::new(Device {
        tag: tag.clone(),
        mmio,
        _bar: bar_mapping,
        ctrl,
        _ctrl_mapping: ctrl_mapping,
        ctrl_physical,
        out_ring,
        in_ring,
        completion: Event::new(),
    });

    let irq_device = device.clone();
    hel::spawn(async move {
        if let Err(e) = handle_irqs(irq_device, irq).await {
            eprintln!("dmalog: IRQ handling failed: {e:?}");
        }
    });

    let input_device = device.clone();
    hel::spawn(async move {
        if let Err(e) = serve_input(input_device, input).await {
            eprintln!("dmalog: Serving input failed: {e:?}");
        }
    });

    serve_output(device, output).await
}

fn property_equals(event: &mbus::EnumerationEvent, name: &str, expected: &str) -> bool {
    matches!(event.properties().get(name), Some(Item::String(value)) if value == expected)
}

async fn run() -> Result<()> {
    let filter = mbus::Filter::Conjunction(&[
        mbus::Filter::Equals("pci-vendor", "1234"),
        mbus::Filter::Equals("pci-device", "69e8"),
    ]);
    let mut enumerator = mbus::Enumerator::new(filter);
    loop {
        let (_overflow, events) = enumerator.next_events().await?;
        for event in events {
            if event.event_type() != EventType::Created
                || !property_equals(&event, "pci-revision", "12")
            {
                continue;
            }
            let entity = event.entity();
            hel::spawn(async move {
                if let Err(e) = bind_device(entity).await {
                    eprintln!("dmalog: Failed to bind device: {e:?}");
                }
            });
        }
    }
}

fn main() -> Result<()> {
    hel::block_on(run())?
}
