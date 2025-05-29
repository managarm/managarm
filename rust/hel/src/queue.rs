use std::ffi::{c_int, c_uint};
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
    ring_shift: usize,
    num_chunks: usize,
    chunks_offset: usize,
    reserved_per_chunk: usize,
    // Queue state
    active_chunks: usize,
    retrieve_index: usize,
    next_index: usize,
    last_progress: usize,
    had_waiters: bool,
    ref_counts: Box<[usize]>,
    // Resources
    handle: Handle,
    mapping: Mapping<hel_sys::HelQueue>,
}

impl Queue {
    /// Creates a new queue with the given parameters.
    pub fn new(ring_shift: usize, num_chunks: usize, chunk_size: usize) -> Result<Self> {
        let mut queue_params = hel_sys::HelQueueParameters {
            flags: 0,
            ringShift: ring_shift as c_uint,
            numChunks: num_chunks as c_uint,
            chunkSize: chunk_size,
        };

        let mut raw_handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;

        hel_check(unsafe { hel_sys::helCreateQueue(&mut queue_params, &mut raw_handle) })?;

        let chunks_offset = (size_of::<hel_sys::HelQueue>() + (size_of::<c_int>() << ring_shift))
            .next_multiple_of(64);
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

        Ok(Self {
            ring_shift,
            num_chunks,
            chunks_offset,
            reserved_per_chunk,
            active_chunks: 0,
            retrieve_index: 0,
            next_index: 0,
            last_progress: 0,
            had_waiters: false,
            ref_counts: vec![0; num_chunks].into_boxed_slice(),
            handle,
            mapping,
        })
    }

    /// Returns a reference to the queue's handle.
    /// This handle can be used to submit work to the queue.
    pub fn handle(&self) -> &Handle {
        &self.handle
    }

    /// Waits for a completion on the queue.
    /// This function blocks until a completion is available.
    pub fn wait(&mut self) -> Result<QueueElement> {
        loop {
            if self.retrieve_index == self.next_index {
                self.reset_and_enqueue_chunk(self.active_chunks)?;
                self.active_chunks += 1;

                continue;
            } else if self.had_waiters && self.active_chunks < (1 << self.ring_shift) {
                self.reset_and_enqueue_chunk(self.active_chunks)?;
                self.active_chunks += 1;
                self.had_waiters = false;
            }

            if self.wait_progress_futex()? {
                self.release_chunk(self.get_index(self.retrieve_index) as usize)?;

                self.last_progress = 0;
                self.retrieve_index = (self.retrieve_index + 1) & hel_sys::kHelHeadMask as usize;

                continue;
            }

            let chunk_num = self.retrieve_index & ((1 << self.ring_shift) - 1);
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

    /// Resets the progress futex of the given chunk and marks it as
    /// available for the kernel to use.
    fn reset_and_enqueue_chunk(&mut self, chunk_num: usize) -> Result<()> {
        self.get_chunk(chunk_num)
            .progress_futex()
            .store(0, Ordering::SeqCst);

        self.set_index(self.next_index, chunk_num as i32);

        self.next_index = (self.next_index + 1) & hel_sys::kHelHeadMask as usize;

        self.wake_head_futex()?;

        self.ref_counts[chunk_num] = 1;

        Ok(())
    }

    /// Increments the reference count of the given chunk.
    fn retain_chunk(&mut self, chunk_num: usize) {
        assert!(self.ref_counts[chunk_num] > 0);

        self.ref_counts[chunk_num] += 1;
    }

    /// Drops the reference count of the given chunk, and if it
    /// reaches one it resets the chunk and marks it as available for use.
    fn release_chunk(&mut self, chunk_num: usize) -> Result<()> {
        assert!(self.ref_counts[chunk_num] > 0);

        let ref_count = self.ref_counts[chunk_num];

        self.ref_counts[chunk_num] -= 1;

        if ref_count > 1 {
            Ok(())
        } else {
            self.reset_and_enqueue_chunk(chunk_num)
        }
    }

    /// Wakes up the head futex if needed.
    fn wake_head_futex(&mut self) -> Result<()> {
        let new_futex = self.next_index as i32;
        let old_futex = self.head_futex().swap(new_futex, Ordering::Release);

        if old_futex & hel_sys::kHelHeadWaiters != 0 {
            hel_check(unsafe { hel_sys::helFutexWake(self.head_futex().as_ptr()) })?;

            self.had_waiters = true;
        }

        Ok(())
    }

    fn wait_progress_futex(&mut self) -> Result<bool> {
        loop {
            let mut futex = self
                .get_chunk(self.retrieve_index)
                .progress_futex()
                .load(Ordering::Acquire);

            loop {
                if self.last_progress as i32 != (futex & hel_sys::kHelProgressMask) {
                    return Ok(false);
                } else if futex & hel_sys::kHelProgressDone != 0 {
                    return Ok(true);
                }

                if futex & hel_sys::kHelProgressWaiters != 0 {
                    break; // Waiters bit is already set (in a previous iteration).
                }

                let new_futex = self.last_progress as i32 | hel_sys::kHelProgressWaiters;

                match self
                    .get_chunk(self.retrieve_index)
                    .progress_futex()
                    .compare_exchange(futex, new_futex, Ordering::Acquire, Ordering::Acquire)
                {
                    Ok(_) => break,
                    Err(old_futex) => futex = old_futex,
                }
            }

            hel_check(unsafe {
                hel_sys::helFutexWait(
                    self.get_chunk(self.retrieve_index)
                        .progress_futex()
                        .as_ptr(),
                    self.last_progress as i32 | hel_sys::kHelProgressWaiters,
                    -1,
                )
            })?;
        }
    }

    /// Returns a reference to the head futex of the queue.
    fn head_futex(&mut self) -> &AtomicI32 {
        // SAFETY: The head futex is always accessed atomically
        // by the kernel, so it's safe to obtain a pointer to it.
        unsafe {
            AtomicI32::from_ptr(
                self.mapping
                    .as_ptr()
                    .unwrap()
                    .byte_add(offset_of!(hel_sys::HelQueue, headFutex))
                    .as_ptr()
                    .cast(),
            )
        }
    }

    /// Returns a reference to the index queue.
    fn index_queue(&self) -> &[i32] {
        let pointer = unsafe {
            self.mapping
                .as_ptr()
                .unwrap()
                .byte_add(offset_of!(hel_sys::HelQueue, indexQueue))
                .cast::<i32>()
        };

        // SAFETY: The index queue is never written to by the kernel,
        // so it's safe to obtain a slice to the it.
        unsafe { std::slice::from_raw_parts(pointer.as_ptr(), 1 << self.ring_shift) }
    }

    /// Returns a mutable reference to the index queue.
    fn index_queue_mut(&mut self) -> &mut [i32] {
        let pointer = unsafe {
            self.mapping
                .as_ptr()
                .unwrap()
                .byte_add(offset_of!(hel_sys::HelQueue, indexQueue))
                .cast::<i32>()
        };

        // SAFETY: The index queue is never written to by the kernel,
        // so it's safe to obtain a mutable slice to it.
        unsafe { std::slice::from_raw_parts_mut(pointer.as_ptr(), 1 << self.ring_shift) }
    }

    fn get_index(&self, index: usize) -> i32 {
        let index_queue = self.index_queue();

        index_queue[index & ((1 << self.ring_shift) - 1)]
    }

    fn set_index(&mut self, index: usize, value: i32) {
        let ring_shift = self.ring_shift;
        let index_queue = self.index_queue_mut();

        index_queue[index & ((1 << ring_shift) - 1)] = value;
    }

    /// Returns a reference to the chunk at the given index.
    /// The index is wrapped around the number of chunks to allow
    /// for easier indexing since the queue is circular.
    fn get_chunk(&mut self, index: usize) -> Chunk {
        let index = index & ((1 << self.ring_shift) - 1);

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
