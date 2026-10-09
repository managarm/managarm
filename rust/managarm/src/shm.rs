//! Shared memory communication (currently ring buffers); see `protocols/shm/shm.bragi` for
//! the protocol and `protocols/shm/include/protocols/shm/core.hpp` for the reference
//! implementation.

use std::ptr::NonNull;
use std::sync::atomic::{AtomicU64, Ordering};

use hel::{Handle, Mapping, MappingFlags};

bragi::include_binding!(mod bindings = "shm.rs");

/// Offset of the data area within the producer memory.
pub const DATA_OFFSET: usize = 0x1000;
pub const CONSUMER_MEMORY_SIZE: usize = 0x1000;

/// Rights that a peer needs to map a memory object that it may write to.
pub const WRITER_RIGHTS: u32 =
    hel_sys::kHelRightRead | hel_sys::kHelRightWrite | hel_sys::kHelRightAssign;
/// Rights that a peer needs to map a memory object that it may not write to.
pub const READER_RIGHTS: u32 = hel_sys::kHelRightRead | hel_sys::kHelRightAssign;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// The producer waits until the consumer frees up space.
    Reliable,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Framing {
    /// The ring carries a byte stream without any structure.
    Stream,
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("the other side of the ring does not adhere to the protocol")]
    ProtocolViolation,
    #[error("the other side of the ring is gone")]
    PeerClosed,
    #[error("the descriptors that were received for the ring are unusable")]
    HandshakeFailed,
    #[error("the provider's ring does not match the requested ring")]
    Mismatch,
    #[error(transparent)]
    Hel(#[from] hel::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

/// Header of the producer memory. Only written by the producer.
#[repr(C)]
struct ProducerHeader {
    head: AtomicU64,
    _reserved: AtomicU64,
    flushed: AtomicU64,
    wake_at: AtomicU64,
}

/// Header of the consumer memory. Only written by the consumer.
#[repr(C)]
struct ConsumerHeader {
    consumed: AtomicU64,
    wake_at: AtomicU64,
}

fn passes_wake_at(wake_at: u64, old_position: u64, new_position: u64) -> bool {
    old_position < wake_at && wake_at <= new_position
}

// ----------------------------------------------------------------------------
// Kick events.
// ----------------------------------------------------------------------------

// Kicks the other side of a ring by raising the event that we own.
// Never blocks; kicks that the other side did not handle yet are coalesced.
fn kick(event: &Handle) {
    // This only fails if the other side is gone; it does not miss the kick in that case.
    let _ = hel::raise_event(event);
}

// Waits until the other side raised its event more than `sequence` times or until it is gone.
// Returns the number of kicks that were received so far.
async fn await_kick(event: &Handle, sequence: u64) -> Result<u64> {
    match hel::await_event(event, sequence).await {
        Ok(sequence) => Ok(sequence),
        Err(hel::Error::EndOfLane) => Err(Error::PeerClosed),
        Err(e) => Err(e.into()),
    }
}

fn map_memory(memory: &Handle, size: usize, writable: bool) -> Result<(Mapping<u8>, NonNull<u8>)> {
    let mut flags = MappingFlags::READ;
    if writable {
        flags |= MappingFlags::WRITE;
    }
    // SAFETY: the mapping is only accessed through raw pointers and atomics.
    let mapping = unsafe { Mapping::<u8>::new(memory, None, 0, size, flags) }?;
    let pointer = unsafe { mapping.as_ptr() }.expect("mapping is not mapped");
    Ok((mapping, pointer))
}

// ----------------------------------------------------------------------------
// Producer.
// ----------------------------------------------------------------------------

pub struct Producer {
    size: usize,
    _producer_memory: Handle,
    _consumer_memory: Handle,
    _producer_mapping: Mapping<u8>,
    _consumer_mapping: Mapping<u8>,
    header: NonNull<ProducerHeader>,
    consumer_header: NonNull<ConsumerHeader>,
    data: NonNull<u8>,
    // Raised by us to kick the consumer.
    event: Handle,
    // Raised by the consumer to kick us.
    consumer_event: Handle,
    // Number of kicks that we observed so far.
    kick_sequence: u64,
    failed: bool,

    // The shared copies of our own positions are never read back.
    head: u64,
    flushed: u64,
    // Validated copy of the consumer's position.
    consumed: u64,
}

impl Producer {
    fn new(
        size: usize,
        producer_memory: Handle,
        consumer_memory: Handle,
        producer_event: Handle,
        consumer_event: Handle,
    ) -> Result<Producer> {
        let (producer_mapping, producer_pointer) =
            map_memory(&producer_memory, DATA_OFFSET + size, true)?;
        let (consumer_mapping, consumer_pointer) =
            map_memory(&consumer_memory, CONSUMER_MEMORY_SIZE, false)?;
        Ok(Producer {
            size,
            _producer_memory: producer_memory,
            _consumer_memory: consumer_memory,
            _producer_mapping: producer_mapping,
            _consumer_mapping: consumer_mapping,
            header: producer_pointer.cast(),
            consumer_header: consumer_pointer.cast(),
            data: unsafe { producer_pointer.add(DATA_OFFSET) },
            event: producer_event,
            consumer_event,
            kick_sequence: 0,
            failed: false,
            head: 0,
            flushed: 0,
            consumed: 0,
        })
    }

    fn header(&self) -> &ProducerHeader {
        unsafe { self.header.as_ref() }
    }

    fn consumer_header(&self) -> &ConsumerHeader {
        unsafe { self.consumer_header.as_ref() }
    }

    /// Size of the data area, as chosen by the provider.
    pub fn size(&self) -> usize {
        self.size
    }

    /// Position of the next byte that is produced.
    pub fn head(&self) -> u64 {
        self.head
    }

    /// Base of the data area; the offset of a position is the position modulo the ring size.
    pub fn data_ptr(&self) -> NonNull<u8> {
        self.data
    }

    // Picks up the progress of the consumer.
    fn refresh(&mut self) -> Result<()> {
        // SeqCst since wait_for_space() relies on the ordering against the store to wake_at.
        let consumed = self.consumer_header().consumed.load(Ordering::SeqCst);
        if consumed < self.consumed || consumed > self.head {
            self.failed = true;
            return Err(Error::ProtocolViolation);
        }
        self.consumed = consumed;
        Ok(())
    }

    /// Free space as of the last `wait_for_space()`.
    pub fn free_size(&self) -> usize {
        self.size - (self.head - self.consumed) as usize
    }

    /// Commits bytes that were written to the data area at `head()`.
    pub fn produce(&mut self, size: usize) {
        assert!(size <= self.free_size());
        let old_head = self.head;
        self.head += size as u64;
        // Commit the operation *after* writing to the ring.
        self.header().head.store(self.head, Ordering::SeqCst);

        let wake_at = self.consumer_header().wake_at.load(Ordering::SeqCst);
        if passes_wake_at(wake_at, old_head, self.head) {
            kick(&self.event);
        }
    }

    /// Waits until the given amount of space is free.
    pub async fn wait_for_space(&mut self, size: usize) -> Result<()> {
        assert!(size <= self.size);
        loop {
            if self.failed {
                return Err(Error::ProtocolViolation);
            }

            self.refresh()?;
            if self.free_size() >= size {
                return Ok(());
            }
            let wake_at = self.head + size as u64 - self.size as u64;
            self.header().wake_at.store(wake_at, Ordering::SeqCst);
            self.refresh()?;
            if self.free_size() >= size {
                return Ok(());
            }

            // Kicks that we did not observe before the checks above complete this immediately.
            self.kick_sequence = await_kick(&self.consumer_event, self.kick_sequence).await?;
        }
    }

    /// Asks the consumer to process all committed data regardless of its watermark.
    pub fn flush(&mut self) {
        if self.flushed == self.head {
            return;
        }
        self.flushed = self.head;
        self.header().flushed.store(self.flushed, Ordering::SeqCst);

        let wake_at = self.consumer_header().wake_at.load(Ordering::SeqCst);
        if wake_at > self.head {
            kick(&self.event);
        }
    }
}

// ----------------------------------------------------------------------------
// Consumer.
// ----------------------------------------------------------------------------

pub struct Consumer {
    size: usize,
    _producer_memory: Handle,
    _consumer_memory: Handle,
    _producer_mapping: Mapping<u8>,
    _consumer_mapping: Mapping<u8>,
    producer_header: NonNull<ProducerHeader>,
    header: NonNull<ConsumerHeader>,
    data: NonNull<u8>,
    // Raised by the producer to kick us.
    producer_event: Handle,
    // Raised by us to kick the producer.
    event: Handle,
    // Number of kicks that we observed so far.
    kick_sequence: u64,
    failed: bool,

    // The shared copy of our own position is never read back.
    position: u64,
    // Validated copy of the producer's position.
    head: u64,
}

impl Consumer {
    fn new(
        size: usize,
        producer_memory: Handle,
        consumer_memory: Handle,
        producer_event: Handle,
        consumer_event: Handle,
    ) -> Result<Consumer> {
        let (producer_mapping, producer_pointer) =
            map_memory(&producer_memory, DATA_OFFSET + size, false)?;
        let (consumer_mapping, consumer_pointer) =
            map_memory(&consumer_memory, CONSUMER_MEMORY_SIZE, true)?;
        Ok(Consumer {
            size,
            _producer_memory: producer_memory,
            _consumer_memory: consumer_memory,
            _producer_mapping: producer_mapping,
            _consumer_mapping: consumer_mapping,
            producer_header: producer_pointer.cast(),
            header: consumer_pointer.cast(),
            data: unsafe { producer_pointer.add(DATA_OFFSET) },
            producer_event,
            event: consumer_event,
            kick_sequence: 0,
            failed: false,
            position: 0,
            head: 0,
        })
    }

    fn producer_header(&self) -> &ProducerHeader {
        unsafe { self.producer_header.as_ref() }
    }

    fn header(&self) -> &ConsumerHeader {
        unsafe { self.header.as_ref() }
    }

    /// Size of the data area, as chosen by the provider.
    pub fn size(&self) -> usize {
        self.size
    }

    /// Position of the next byte that is consumed.
    pub fn position(&self) -> u64 {
        self.position
    }

    /// Base of the data area; the offset of a position is the position modulo the ring size.
    pub fn data_ptr(&self) -> NonNull<u8> {
        self.data
    }

    // Picks up the progress of the producer.
    fn refresh(&mut self) -> Result<()> {
        let head = self.producer_header().head.load(Ordering::SeqCst);
        if head < self.head || head > self.position + self.size as u64 {
            self.failed = true;
            return Err(Error::ProtocolViolation);
        }
        self.head = head;
        Ok(())
    }

    /// Number of available bytes as of the last `wait_for_data()`.
    pub fn available_size(&self) -> usize {
        (self.head - self.position) as usize
    }

    /// Frees bytes that were taken from the data area at `position()`.
    pub fn consume(&mut self, size: usize) {
        assert!(size <= self.available_size());
        let old_position = self.position;
        self.position += size as u64;
        self.header()
            .consumed
            .store(self.position, Ordering::SeqCst);

        let wake_at = self.producer_header().wake_at.load(Ordering::SeqCst);
        if passes_wake_at(wake_at, old_position, self.position) {
            kick(&self.event);
        }
    }

    /// Waits until `watermark` bytes are available or until the producer flushes.
    pub async fn wait_for_data(&mut self, watermark: usize) -> Result<()> {
        assert!(watermark > 0 && watermark <= self.size);
        loop {
            if self.failed {
                return Err(Error::ProtocolViolation);
            }

            let wake_at = self.position + watermark as u64;
            self.header().wake_at.store(wake_at, Ordering::SeqCst);

            self.refresh()?;
            if self.head >= wake_at {
                return Ok(());
            }
            if self.producer_header().flushed.load(Ordering::SeqCst) > self.position {
                return Ok(());
            }

            // Kicks that we did not observe before the checks above complete this immediately.
            self.kick_sequence = await_kick(&self.producer_event, self.kick_sequence).await?;
        }
    }
}

// ----------------------------------------------------------------------------
// Establishing rings.
// ----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Role {
    Producer,
    Consumer,
}

// Handles of a ring that were pulled from the provider.
struct RingHandles {
    size: usize,
    producer_memory: Handle,
    producer_event: Handle,
    consumer_memory: Handle,
    consumer_event: Handle,
}

async fn pull_descriptor(conversation: &Handle, rights: u32) -> Result<Handle> {
    hel::submit_async(conversation, hel::PullDescriptor::new(rights))
        .await??
        .ok_or(Error::HandshakeFailed)
}

// Do not trust the peer to pass memory objects of the correct size.
fn check_memory_size(memory: &Handle, size: usize) -> Result<()> {
    let mut actual_size = 0;
    let error = unsafe { hel_sys::helMemoryInfo(memory.handle(), &mut actual_size) };
    if error as u32 != hel_sys::kHelErrNone || actual_size < size {
        return Err(Error::HandshakeFailed);
    }
    Ok(())
}

// Pulls the memory and the kick event of one side of the ring; `owns` if we play that side.
async fn pull_side(conversation: &Handle, owns: bool) -> Result<(Handle, Handle)> {
    let (memory_rights, event_rights) = if owns {
        (WRITER_RIGHTS, hel_sys::kHelRightSignal)
    } else {
        (READER_RIGHTS, hel_sys::kHelRightWait)
    };
    let memory = pull_descriptor(conversation, memory_rights).await?;
    let event = pull_descriptor(conversation, event_rights).await?;
    Ok((memory, event))
}

// Sends the EstablishRingRequest and returns the ring size from the provider's
// EstablishRingResponse.
async fn request_ring(
    conversation: &Handle,
    mode: Mode,
    framing: Framing,
    role: Role,
) -> Result<usize> {
    let mode = match mode {
        Mode::Reliable => bindings::Mode::Reliable,
    };
    let framing = match framing {
        Framing::Stream => bindings::Framing::Stream,
    };
    let role = match role {
        Role::Producer => bindings::Role::Producer,
        Role::Consumer => bindings::Role::Consumer,
    };
    let request = bindings::EstablishRingRequest::new(mode, framing, role);
    let head = bragi::head_to_bytes(&request).expect("failed to serialize an EstablishRingRequest");
    let (send_head, recv_head) = hel::submit_async(
        conversation,
        (hel::SendBuffer::new(&head), hel::ReceiveInline),
    )
    .await?;
    send_head?;

    let response: bindings::EstablishRingResponse =
        bragi::head_from_bytes(&recv_head?).map_err(|_| Error::HandshakeFailed)?;
    if response.error() != bindings::Error::Success {
        return Err(Error::Mismatch);
    }
    match usize::try_from(response.ring_size()) {
        Ok(size) if size >= DATA_OFFSET && size.is_power_of_two() => Ok(size),
        _ => Err(Error::HandshakeFailed),
    }
}

// Establishes a ring over a conversation lane and pulls the descriptors that the provider pushes.
async fn establish_ring(
    conversation: &Handle,
    mode: Mode,
    framing: Framing,
    role: Role,
) -> Result<RingHandles> {
    let size = request_ring(conversation, mode, framing, role).await?;

    let (producer_memory, producer_event) = pull_side(conversation, role == Role::Producer).await?;
    let (consumer_memory, consumer_event) = pull_side(conversation, role == Role::Consumer).await?;
    check_memory_size(&producer_memory, DATA_OFFSET + size)?;
    check_memory_size(&consumer_memory, CONSUMER_MEMORY_SIZE)?;

    Ok(RingHandles {
        size,
        producer_memory,
        producer_event,
        consumer_memory,
        consumer_event,
    })
}

impl Producer {
    /// Establishes a ring that we produce to over a conversation lane, as the host protocol
    /// determines it. The provider chooses the size of the ring.
    pub async fn establish(
        conversation: &Handle,
        mode: Mode,
        framing: Framing,
    ) -> Result<Producer> {
        let handles = establish_ring(conversation, mode, framing, Role::Producer).await?;
        Producer::new(
            handles.size,
            handles.producer_memory,
            handles.consumer_memory,
            handles.producer_event,
            handles.consumer_event,
        )
    }
}

impl Consumer {
    /// Establishes a ring that we consume from over a conversation lane, as the host protocol
    /// determines it. The provider chooses the size of the ring.
    pub async fn establish(
        conversation: &Handle,
        mode: Mode,
        framing: Framing,
    ) -> Result<Consumer> {
        let handles = establish_ring(conversation, mode, framing, Role::Consumer).await?;
        Consumer::new(
            handles.size,
            handles.producer_memory,
            handles.consumer_memory,
            handles.producer_event,
            handles.consumer_event,
        )
    }
}
