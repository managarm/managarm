use std::rc::Rc;

use bragi::Message;
use hel::{Accept, Handle, PushDescriptor, ReceiveInline, SendBuffer, submit_async};

use crate::hw::bindings::Errors;

use super::bindings;
use super::pci::IoType;

#[derive(Debug, Clone, Copy, Default)]
pub struct BarDescriptor {
    pub io_type: IoType,
    pub host_type: IoType,
    pub address: u64,
    pub length: u64,
    pub offset: u32,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct CapDescriptor {
    pub type_: u32,
    pub offset: u64,
    pub length: u64,
}

pub trait PciDevice {
    fn bars(&self) -> [BarDescriptor; 6];
    fn capabilities(&self) -> Vec<CapDescriptor>;
    fn expansion_rom(&self) -> (u64, u64) {
        (0, 0)
    }
    fn num_msis(&self) -> u32 {
        0
    }
    fn msi_x(&self) -> bool {
        false
    }

    fn config_read(&self, offset: u32, size: u32) -> Option<u32>;
    fn config_write(&self, offset: u32, size: u32, word: u32) -> bool;
    fn capability_read(&self, index: i32, offset: u32, size: u32) -> Option<u32>;

    fn access_bar(&self, index: usize) -> hel::Result<Handle>;
    fn access_irq(&self, index: u64) -> hel::Result<Option<Handle>>;
    fn access_expansion_rom(&self) -> hel::Result<Option<Handle>> {
        Ok(None)
    }

    fn enable_busmaster(&self);
    fn enable_irq(&self);
    fn get_dma_space(&self) -> hel::Result<(bool, Handle)> {
        Ok((false, hel::create_dma_space()?))
    }
    fn claim_device(&self) {}
}

fn encode_io_type(t: IoType) -> bindings::IoType {
    match t {
        IoType::None => bindings::IoType::NoBar,
        IoType::Port => bindings::IoType::Port,
        IoType::Memory => bindings::IoType::Memory,
    }
}

fn build_pci_info<D: PciDevice>(device: &D) -> bindings::SvrResponse {
    let mut resp = bindings::SvrResponse::default();
    resp.set_error(Errors::Success);

    if device.num_msis() > 0 {
        resp.set_num_msis(device.num_msis());
        resp.set_msi_x(device.msi_x() as u8);
    }

    let caps = device
        .capabilities()
        .into_iter()
        .map(|cap| {
            let mut msg = bindings::PciCapability::default();
            msg.set_type(cap.type_);
            msg.set_offset(cap.offset);
            msg.set_length(cap.length);
            msg
        })
        .collect();
    resp.set_capabilities(caps);

    let bars = device
        .bars()
        .iter()
        .map(|bar| {
            let mut msg = bindings::PciBar::default();
            msg.set_io_type(encode_io_type(bar.io_type));
            msg.set_host_type(encode_io_type(bar.host_type));
            if bar.host_type != IoType::None {
                msg.set_address(bar.address);
                msg.set_length(bar.length);
                msg.set_offset(bar.offset);
            }
            msg
        })
        .collect();
    resp.set_bars(bars);

    let (rom_addr, rom_len) = device.expansion_rom();
    let mut rom = bindings::PciExpansionRom::default();
    rom.set_address(rom_addr);
    rom.set_length(rom_len);
    resp.set_expansion_rom(rom);

    resp
}

async fn send_response(lane: &Handle, resp: &bindings::SvrResponse) -> hel::Result<()> {
    let (head, tail) = bragi::head_tail_to_bytes(resp).expect("failed to encode hw response");
    let (head, tail) = submit_async(lane, (SendBuffer::new(&head), SendBuffer::new(&tail))).await?;
    head?;
    tail?;
    Ok(())
}

async fn send_response_with_push(
    lane: &Handle,
    resp: &bindings::SvrResponse,
    push: &Handle,
    rights: u32,
) -> hel::Result<()> {
    let (head, tail) = bragi::head_tail_to_bytes(resp).expect("failed to encode hw response");
    let (head, tail, push) = submit_async(
        lane,
        (
            SendBuffer::new(&head),
            SendBuffer::new(&tail),
            PushDescriptor::new(push, rights),
        ),
    )
    .await?;
    (head?, tail?, push?);
    Ok(())
}

impl Into<bindings::SvrResponse> for Errors {
    fn into(self) -> bindings::SvrResponse {
        let mut resp = bindings::SvrResponse::default();
        resp.set_error(self);
        resp
    }
}

async fn handle_one<D: PciDevice>(lane: &Handle, request: &[u8], device: &D) -> hel::Result<()> {
    let preamble = match bragi::preamble_from_bytes(request) {
        Ok(p) => p,
        Err(_) => return Ok(()),
    };
    let id = preamble.id();

    match id {
        bindings::GetPciInfoRequest::MESSAGE_ID => {
            let resp = build_pci_info(device);
            send_response(lane, &resp).await?;
        }
        bindings::AccessBarRequest::MESSAGE_ID => {
            let req: bindings::AccessBarRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            match device.access_bar(req.index() as usize) {
                Ok(handle) => {
                    send_response_with_push(
                        lane,
                        &Errors::Success.into(),
                        &handle,
                        hel_sys::kHelRightRead
                            | hel_sys::kHelRightWrite
                            | hel_sys::kHelRightAssign
                            | hel_sys::kHelRightProvision
                            | hel_sys::kHelRightPin,
                    )
                    .await?
                }
                Err(_) => send_response(lane, &Errors::OutOfBounds.into()).await?,
            }
        }
        bindings::AccessIrqRequest::MESSAGE_ID => {
            let req: bindings::AccessIrqRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            match device.access_irq(req.index()) {
                Ok(Some(handle)) => {
                    send_response_with_push(
                        lane,
                        &Errors::Success.into(),
                        &handle,
                        hel_sys::kHelRightWait | hel_sys::kHelRightSignal,
                    )
                    .await?
                }
                _ => send_response(lane, &Errors::IllegalArguments.into()).await?,
            }
        }
        bindings::AccessExpansionRomRequest::MESSAGE_ID => match device.access_expansion_rom() {
            Ok(Some(handle)) => {
                send_response_with_push(
                    lane,
                    &Errors::Success.into(),
                    &handle,
                    hel_sys::kHelRightRead
                        | hel_sys::kHelRightAssign
                        | hel_sys::kHelRightProvision
                        | hel_sys::kHelRightPin,
                )
                .await?
            }
            _ => send_response(lane, &Errors::DeviceError.into()).await?,
        },
        bindings::LoadPciSpaceRequest::MESSAGE_ID => {
            let req: bindings::LoadPciSpaceRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let mut resp = bindings::SvrResponse::default();
            match device.config_read(req.offset(), req.size()) {
                Some(word) => {
                    resp.set_error(Errors::Success);
                    resp.set_word(word);
                }
                None => resp.set_error(Errors::IllegalArguments),
            }
            send_response(lane, &resp).await?;
        }
        bindings::StorePciSpaceRequest::MESSAGE_ID => {
            let req: bindings::StorePciSpaceRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let error = if device.config_write(req.offset(), req.size(), req.word()) {
                Errors::Success
            } else {
                Errors::IllegalArguments
            };
            send_response(lane, &error.into()).await?;
        }
        bindings::LoadPciCapabilityRequest::MESSAGE_ID => {
            let req: bindings::LoadPciCapabilityRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let mut resp = bindings::SvrResponse::default();
            match device.capability_read(req.index(), req.offset(), req.size()) {
                Some(word) => {
                    resp.set_error(Errors::Success);
                    resp.set_word(word);
                }
                None => resp.set_error(Errors::IllegalArguments),
            }
            send_response(lane, &resp).await?;
        }
        bindings::EnableBusmasterRequest::MESSAGE_ID => {
            device.enable_busmaster();
            send_response(lane, &Errors::Success.into()).await?;
        }
        bindings::EnableBusIrqRequest::MESSAGE_ID => {
            device.enable_irq();
            send_response(lane, &Errors::Success.into()).await?;
        }
        bindings::GetDmaSpaceRequest::MESSAGE_ID => {
            let (iommu_active, space) = device.get_dma_space()?;
            let mut resp = bindings::GetDmaSpaceResponse::default();
            resp.set_iommu_active(iommu_active as i8);
            let head = bragi::head_to_bytes(&resp).expect("failed to encode hw response");
            let (head, push) = submit_async(
                lane,
                (
                    SendBuffer::new(&head),
                    PushDescriptor::new(
                        &space,
                        hel_sys::kHelRightGrant | hel_sys::kHelRightProvision,
                    ),
                ),
            )
            .await?;
            (head?, push?);
        }
        bindings::ClaimDeviceRequest::MESSAGE_ID => {
            device.claim_device();
            send_response(lane, &Errors::Success.into()).await?;
        }
        _ => send_response(lane, &Errors::DeviceError.into()).await?,
    }

    Ok(())
}

pub async fn serve_pci_device<D: PciDevice + 'static>(lane: Handle, device: Rc<D>) {
    loop {
        let (conversation, request) = match submit_async(&lane, Accept::new(ReceiveInline)).await {
            Ok((conversation, request)) => (conversation, request),
            Err(_) => break,
        };

        let conversation = match conversation {
            Ok(Some(conversation)) => conversation,
            _ => break,
        };

        let request = match request {
            Ok(request) => request,
            Err(_) => continue,
        };

        if handle_one(&conversation, &request, device.as_ref())
            .await
            .is_err()
        {
            continue;
        }
    }
}
