use std::ffi::c_uint;
use std::marker::PhantomData;
use std::mem::{MaybeUninit, offset_of};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicI32, Ordering};

use crate::{
    handle::Handle,
    mapping::{Mapping, MappingFlags},
    result::{Result, hel_check},
};

/// Wrapper around a Hel queue chunk.
struct Chunk<'a> {
    _marker: PhantomData<&'a ()>,
    chunk: NonNull<hel_sys::HelChunk>,
}

impl<'a> Chunk<'a> {
    /// Creates a new chunk wrapper.
    fn new(chunk: NonNull<hel_sys::HelChunk>) -> Self {
        Self {
            _marker: PhantomData,
            chunk,
        }
    }

    /// Returns a reference to the chunk's next field.
    fn next(&mut self) -> &'a AtomicI32 {
        // SAFETY: The next field is always accessed atomically
        // by the kernel, so it's safe to obtain a pointer to it.
        unsafe {
            AtomicI32::from_ptr(
                self.chunk
                    .as_ptr()
                    .byte_add(offset_of!(hel_sys::HelChunk, next))
                    .cast(),
            )
        }
    }

    /// Returns a reference to the chunk's progress futex.
    fn progress_futex(&mut self) -> &'a AtomicI32 {
        // SAFETY: The progress futex is always accessed atomically
        // by the kernel, so it's safe to obtain a pointer to it.
        unsafe {
            AtomicI32::from_ptr(
                self.chunk
                    .as_ptr()
                    .byte_add(offset_of!(hel_sys::HelChunk, progressFutex))
                    .cast(),
            )
        }
    }

    /// Returns a pointer to the chunk's buffer.
    fn buffer(&mut self) -> NonNull<()> {
        unsafe { self.chunk.byte_add(size_of::<hel_sys::HelChunk>()).cast() }
    }
}

/// A queue element that can be used to retrieve data from the queue.
pub struct QueueElement<'a> {
    queue: &'a mut Queue,
    data: &'a [MaybeUninit<u8>],
    context: usize,
    chunk_num: usize,
    offset: usize,
}

impl<'a> QueueElement<'a> {
    /// Creates a new queue element.
    fn new(
        queue: &'a mut Queue,
        data: &'a [MaybeUninit<u8>],
        context: usize,
        chunk_num: usize,
    ) -> Self {
        queue.retain_chunk(chunk_num);

        Self {
            queue,
            data,
            context,
            chunk_num,
            offset: 0,
        }
    }

    /// Returns the context associated with this queue element.
    pub fn context(&self) -> usize {
        self.context
    }

    /// Returns a slice of the remaining data of this queue element.
    pub fn data(&self) -> &'a [MaybeUninit<u8>] {
        &self.data[self.offset..]
    }

    /// Advances the offset of this queue element by the given length.
    /// This is used to consume the data of the queue element.
    /// Panics if the offset would exceed the length of the data.
    pub fn advance(&mut self, length: usize) {
        assert!(self.offset + length <= self.data.len());
        self.offset += length;
    }
}

impl Drop for QueueElement<'_> {
    fn drop(&mut self) {
        self.queue
            .release_chunk(self.chunk_num)
            .expect("Failed to release chunk");
    }
}

/// A wrapper around a Hel IPC queue.
/// This queue is used to receive completions for asynchronous
/// submissions.
pub struct Queue {
    // Queue parameters
    num_chunks: usize,
    chunks_offset: usize,
    reserved_per_chunk: usize,
    // Queue state
    /// Chunk that we are currently retrieving from.
    retrieve_chunk: usize,
    /// Tail of the chunk list (where we append new chunks).
    tail_chunk: usize,
    /// Progress into the current chunk.
    last_progress: usize,
    /// Per-chunk reference counts.
    ref_counts: Box<[usize]>,
    // Resources
    handle: Handle,
    mapping: Mapping<hel_sys::HelQueue>,
}

impl Queue {
    /// Creates a new queue with the given parameters.
    pub fn new(num_chunks: usize, chunk_size: usize) -> Result<Self> {
        let mut queue_params = hel_sys::HelQueueParameters {
            flags: 0,
            numChunks: num_chunks as c_uint,
            chunkSize: chunk_size,
        };

        let mut raw_handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;

        hel_check(unsafe { hel_sys::helCreateQueue(&mut queue_params, &mut raw_handle) })?;

        let chunks_offset = size_of::<hel_sys::HelQueue>().next_multiple_of(64);
        let reserved_per_chunk = (size_of::<hel_sys::HelChunk>() + chunk_size).next_multiple_of(64);
        let handle = unsafe { Handle::from_raw(raw_handle) };
        let mapping = unsafe {
            Mapping::new(
                &handle,
                None,
                0,
                (chunks_offset + reserved_per_chunk * num_chunks).next_multiple_of(4096),
                MappingFlags::READ | MappingFlags::WRITE,
            )?
        };

        let mut queue = Self {
            num_chunks,
            chunks_offset,
            reserved_per_chunk,
            retrieve_chunk: 0,
            tail_chunk: 0,
            last_progress: 0,
            ref_counts: vec![0; num_chunks].into_boxed_slice(),
            handle,
            mapping,
        };

        // Reset all chunks.
        for i in 0..num_chunks {
            queue.reset_chunk(i);
        }

        // Set cqFirst to the initial chunk.
        queue
            .cq_first()
            .store(0 | hel_sys::kHelNextPresent, Ordering::Release);

        // Supply the remaining chunks.
        queue.tail_chunk = 0;
        for i in 1..num_chunks {
            queue.supply_chunk(i)?;
        }
        queue.retrieve_chunk = 0;
        queue.wake_kernel_futex()?;

        Ok(queue)
    }

    /// Returns a reference to the queue's handle.
    /// This handle can be used to submit work to the queue.
    pub fn handle(&self) -> &Handle {
        &self.handle
    }

    /// Waits for a completion on the queue.
    /// This function blocks until a completion is available.
    pub fn wait(&mut self) -> Result<QueueElement<'_>> {
        loop {
            if self.wait_progress_futex()? {
                // Chunk is done, move to the next one.
                let cn = self.retrieve_chunk;
                let next = self.get_chunk(cn).next().load(Ordering::Acquire);
                self.surrender(cn)?;

                self.last_progress = 0;
                self.retrieve_chunk = (next & !hel_sys::kHelNextPresent) as usize;

                continue;
            }

            // Dequeue the next element.
            let chunk_num = self.retrieve_chunk;
            let pointer = unsafe {
                self.get_chunk(chunk_num)
                    .buffer()
                    .byte_add(self.last_progress)
            };

            // SAFETY: The reference created here is valid and points to
            // a valid [`hel_sys::HelElement`] structure followed by the
            // data of the element which is guaranteed to be valid and
            // initialized up to the amount of bytes specified in the
            // [`hel_sys::HelElement::length`] field.
            //
            // The element and it's data were written into the shared buffer
            // by the kernel before waking up the futex and it is guaranteed
            // to no longer be accessed by the kernel.
            let element: &hel_sys::HelElement = unsafe { pointer.cast().as_ref() };

            self.last_progress += size_of::<hel_sys::HelElement>();
            self.last_progress += element.length as usize;

            // SAFETY: The data is stored inline after the
            // HelElement header, but it might contain padding
            // bytes which are not initialized, so we use
            // [`MaybeUninit`] to avoid creating a reference
            // to uninitialized memory.
            break Ok(QueueElement::new(
                self,
                unsafe {
                    std::slice::from_raw_parts(
                        pointer
                            .byte_add(size_of::<hel_sys::HelElement>())
                            .cast()
                            .as_ptr(),
                        element.length as usize,
                    )
                },
                element.context as usize,
                chunk_num,
            ));
        }
    }

    /// Decrements the reference count of a chunk, and re-supplies it if it reaches zero.
    fn surrender(&mut self, cn: usize) -> Result<()> {
        assert!(self.ref_counts[cn] > 0);
        self.ref_counts[cn] -= 1;
        if self.ref_counts[cn] > 0 {
            return Ok(());
        }
        self.reset_chunk(cn);
        self.supply_chunk(cn)
    }

    /// Resets a chunk's state.
    fn reset_chunk(&mut self, cn: usize) {
        let mut chunk = self.get_chunk(cn);
        chunk.next().store(0, Ordering::SeqCst);
        chunk.progress_futex().store(0, Ordering::SeqCst);

        self.ref_counts[cn] = 1;
    }

    /// Supplies a chunk to the kernel by linking it to the tail.
    fn supply_chunk(&mut self, cn: usize) -> Result<()> {
        self.get_chunk(self.tail_chunk)
            .next()
            .store((cn as i32) | hel_sys::kHelNextPresent, Ordering::Release);
        self.tail_chunk = cn;
        self.wake_kernel_futex()
    }

    /// Increments the reference count of the given chunk.
    fn retain_chunk(&mut self, chunk_num: usize) {
        assert!(self.ref_counts[chunk_num] > 0);

        self.ref_counts[chunk_num] += 1;
    }

    /// Drops the reference count of the given chunk, and if it
    /// reaches zero it resets the chunk and marks it as available for use.
    fn release_chunk(&mut self, chunk_num: usize) -> Result<()> {
        self.surrender(chunk_num)
    }

    /// Wakes up the kernel if needed.
    fn wake_kernel_futex(&mut self) -> Result<()> {
        let old_futex = self
            .kernel_notify()
            .fetch_or(hel_sys::kHelKernelNotifySupplyCqChunks, Ordering::Release);

        if old_futex & hel_sys::kHelKernelNotifySupplyCqChunks == 0 {
            hel_check(unsafe { hel_sys::helDriveQueue(self.handle.handle(), 0) })?;
        }

        Ok(())
    }

    fn wait_progress_futex(&mut self) -> Result<bool> {
        let check = |this: &mut Self| -> Option<bool> {
            let progress = this
                .get_chunk(this.retrieve_chunk)
                .progress_futex()
                .load(Ordering::Acquire);

            if this.last_progress as i32 != (progress & hel_sys::kHelProgressMask) {
                Some(false)
            } else if progress & hel_sys::kHelProgressDone != 0 {
                Some(true)
            } else {
                None
            }
        };

        if let Some(done) = check(self) {
            return Ok(done);
        }

        loop {
            self.user_notify()
                .fetch_and(!hel_sys::kHelUserNotifyCqProgress, Ordering::Acquire);

            if let Some(done) = check(self) {
                return Ok(done);
            }

            let res = hel_check(unsafe {
                hel_sys::helDriveQueue(self.handle.handle(), hel_sys::kHelDriveWaitCqProgress)
            });
            match res {
                Err(crate::Error::Cancelled) => continue,
                _ => (),
            };
            res?;
        }
    }

    /// Returns a reference to the cqFirst field of the queue.
    fn cq_first(&mut self) -> &AtomicI32 {
        // SAFETY: The cqFirst field is always accessed atomically
        // by the kernel, so it's safe to obtain a pointer to it.
        unsafe {
            AtomicI32::from_ptr(
                self.mapping
                    .as_ptr()
                    .unwrap()
                    .byte_add(offset_of!(hel_sys::HelQueue, cqFirst))
                    .as_ptr()
                    .cast(),
            )
        }
    }

    /// Returns a reference to the userNotify futex of the queue.
    fn user_notify(&mut self) -> &AtomicI32 {
        // SAFETY: The userNotify futex is always accessed atomically
        // by the kernel, so it's safe to obtain a pointer to it.
        unsafe {
            AtomicI32::from_ptr(
                self.mapping
                    .as_ptr()
                    .unwrap()
                    .byte_add(offset_of!(hel_sys::HelQueue, userNotify))
                    .as_ptr()
                    .cast(),
            )
        }
    }

    /// Returns a reference to the kernelNotify futex of the queue.
    fn kernel_notify(&mut self) -> &AtomicI32 {
        // SAFETY: The kernelNotify futex is always accessed atomically
        // by the kernel, so it's safe to obtain a pointer to it.
        unsafe {
            AtomicI32::from_ptr(
                self.mapping
                    .as_ptr()
                    .unwrap()
                    .byte_add(offset_of!(hel_sys::HelQueue, kernelNotify))
                    .as_ptr()
                    .cast(),
            )
        }
    }

    /// Returns a reference to the chunk at the given index.
    fn get_chunk(&mut self, index: usize) -> Chunk<'_> {
        assert!(index < self.num_chunks);

        Chunk::new(unsafe {
            self.mapping
                .as_ptr()
                .unwrap()
                .byte_add(self.chunks_offset + index * self.reserved_per_chunk)
                .cast()
        })
    }
}
