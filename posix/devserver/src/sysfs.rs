//! Implements the sysfs file system.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex, Weak};

use async_trait::async_trait;
use bragi::Message;
use hel::Handle;
use managarm::fs;
use managarm::fs::bindings as fs_proto;

use crate::EXPECT_LOCK;

const SYSFS_MAGIC: u32 = 0x62656572;

/// Handles the read/write operations of a sysfs attribute (i.e., a regular file).
#[async_trait(?Send)]
pub trait Attribute: Send + Sync {
    fn writable(&self) -> bool {
        false
    }

    fn size(&self) -> u64 {
        /// Some files report actual sizes but most files default to page size.
        4096
    }

    async fn show(&self) -> Result<Vec<u8>, fs::server::Error>;

    async fn store(&self, _data: &[u8]) -> Result<(), fs::server::Error> {
        Err(fs::server::Error::IllegalOperationTarget)
    }

    async fn access_memory(&self) -> Result<Handle, fs::server::Error> {
        Err(fs::server::Error::IllegalOperationTarget)
    }
}

/// An attribute with fixed contents.
pub struct StaticAttribute {
    data: Vec<u8>,
    size: u64,
}

impl StaticAttribute {
    pub fn new(data: impl Into<Vec<u8>>) -> Arc<Self> {
        Arc::new(Self {
            data: data.into(),
            size: 4096,
        })
    }

    /// A binary attribute that reports its actual size.
    pub fn new_sized(data: impl Into<Vec<u8>>) -> Arc<Self> {
        let data = data.into();
        Arc::new(Self {
            size: data.len() as u64,
            data,
        })
    }
}

#[async_trait(?Send)]
impl Attribute for StaticAttribute {
    fn size(&self) -> u64 {
        self.size
    }

    async fn show(&self) -> Result<Vec<u8>, fs::server::Error> {
        Ok(self.data.clone())
    }
}

#[derive(Debug, thiserror::Error)]
pub enum TreeError {
    #[error("sysfs entry {path} exists")]
    Exists { path: String },
    #[error("sysfs node {path} is not a directory")]
    NotADirectory { path: String },
}

pub const ROOT_INODE: i64 = 1;

// Note: the allocator is global and not per sysfs superblock.
//       That does not run into issues since the inode space is large enough.
//       Each superblock still has the root inode at 1.
fn allocate_inode() -> i64 {
    static NEXT: AtomicI64 = AtomicI64::new(2);
    NEXT.fetch_add(1, Ordering::Relaxed)
}

/// (parent, name) pair of a node in sysfs.
#[derive(Default)]
struct Location {
    parent: Weak<SysfsNode>,
    name: String,
}

pub struct SysfsNode {
    ino: i64,
    self_ref: Weak<SysfsNode>,
    // Ordered after `NodeKind::Directory::entries`.
    location: Mutex<Location>,
    kind: NodeKind,
}

enum NodeKind {
    Directory {
        entries: Mutex<BTreeMap<String, Arc<SysfsNode>>>,
    },
    Symlink {
        target: Weak<SysfsNode>,
    },
    Attribute {
        attr: Arc<dyn Attribute>,
    },
}

impl SysfsNode {
    fn new(kind: NodeKind, ino: i64) -> Arc<Self> {
        Arc::new_cyclic(|self_ref| Self {
            ino,
            self_ref: self_ref.clone(),
            location: Mutex::new(Location::default()),
            kind,
        })
    }

    fn new_directory(ino: i64) -> Arc<Self> {
        Self::new(
            NodeKind::Directory {
                entries: Mutex::new(BTreeMap::new()),
            },
            ino,
        )
    }

    pub fn new_root() -> Arc<Self> {
        Self::new_directory(ROOT_INODE)
    }

    pub fn is_directory(&self) -> bool {
        matches!(self.kind, NodeKind::Directory { .. })
    }

    pub fn parent(&self) -> Option<Arc<SysfsNode>> {
        self.location.lock().expect(EXPECT_LOCK).parent.upgrade()
    }

    fn entries(&self) -> Result<&Mutex<BTreeMap<String, Arc<SysfsNode>>>, TreeError> {
        match &self.kind {
            NodeKind::Directory { entries, .. } => Ok(entries),
            _ => Err(TreeError::NotADirectory {
                path: self.sysfs_path(),
            }),
        }
    }

    fn insert(self: &Arc<Self>, name: &str, node: Arc<SysfsNode>) -> Result<(), TreeError> {
        let mut entries = self.entries()?.lock().expect(EXPECT_LOCK);
        if entries.contains_key(name) {
            return Err(TreeError::Exists {
                path: format!("{}/{name}", self.sysfs_path()),
            });
        }
        self.attach(&mut entries, name, node);
        Ok(())
    }

    fn attach(
        self: &Arc<Self>,
        entries: &mut BTreeMap<String, Arc<SysfsNode>>,
        name: &str,
        node: Arc<SysfsNode>,
    ) {
        *node.location.lock().expect(EXPECT_LOCK) = Location {
            parent: self.self_ref.clone(),
            name: name.to_string(),
        };
        entries.insert(name.to_string(), node);
    }

    pub fn lookup(&self, name: &str) -> Option<Arc<SysfsNode>> {
        match &self.kind {
            NodeKind::Directory { entries, .. } => {
                entries.lock().expect(EXPECT_LOCK).get(name).cloned()
            }
            _ => None,
        }
    }

    /// Creates a directory or fails if it already exists.
    pub fn create_dir(self: &Arc<Self>, name: &str) -> Result<Arc<SysfsNode>, TreeError> {
        let node = Self::new_directory(allocate_inode());
        self.insert(name, node.clone())?;
        Ok(node)
    }

    /// Gets or creates a directory.
    pub fn dir(self: &Arc<Self>, name: &str) -> Result<Arc<SysfsNode>, TreeError> {
        let mut entries = self.entries()?.lock().expect(EXPECT_LOCK);
        if let Some(existing) = entries.get(name) {
            if !existing.is_directory() {
                return Err(TreeError::NotADirectory {
                    path: existing.sysfs_path(),
                });
            }
            return Ok(existing.clone());
        }
        let node = Self::new_directory(allocate_inode());
        self.attach(&mut entries, name, node.clone());
        Ok(node)
    }

    pub fn create_link(
        self: &Arc<Self>,
        name: &str,
        target: &Arc<SysfsNode>,
    ) -> Result<(), TreeError> {
        let node = Self::new(
            NodeKind::Symlink {
                target: target.self_ref.clone(),
            },
            allocate_inode(),
        );
        self.insert(name, node)
    }

    pub fn create_attr(
        self: &Arc<Self>,
        name: &str,
        attr: Arc<dyn Attribute>,
    ) -> Result<(), TreeError> {
        let node = Self::new(NodeKind::Attribute { attr }, allocate_inode());
        self.insert(name, node)
    }

    pub fn file_type(&self) -> fs::server::FileType {
        match &self.kind {
            NodeKind::Directory { .. } => fs::server::FileType::DIRECTORY,
            NodeKind::Symlink { .. } => fs::server::FileType::SYMLINK,
            NodeKind::Attribute { .. } => fs::server::FileType::REGULAR,
        }
    }

    /// Path components from the root (exclusive) to this node (inclusive).
    fn path_segments(&self) -> Vec<String> {
        let mut segments = Vec::new();
        let mut current = self.self_ref.upgrade().unwrap();
        loop {
            let (parent, name) = {
                let location = current.location.lock().expect(EXPECT_LOCK);
                (location.parent.upgrade(), location.name.clone())
            };
            let Some(parent) = parent else {
                break;
            };
            segments.push(name);
            current = parent;
        }
        segments.reverse();
        segments
    }

    /// Path of this node relative to the sysfs root, e.g. `devices/pci0000:00`.
    pub fn sysfs_path(&self) -> String {
        self.path_segments().join("/")
    }

    fn symlink_target(&self) -> Result<String, fs::server::Error> {
        let NodeKind::Symlink { target } = &self.kind else {
            return Err(fs::server::Error::IllegalOperationTarget);
        };
        // TODO: readlink() on a symlink should succeed even if the target does not exist.
        //       Re-visit the ownership here or snapshot the target.
        //       Note that this can only happen due to races when a device is removed.
        //       posix-subsystem has the same ownership and panics if this happens.
        let target = target.upgrade().ok_or(fs::server::Error::FileNotFound)?;

        let from = match self.parent() {
            Some(parent) => parent.path_segments(),
            None => Vec::new(),
        };
        let to = target.path_segments();
        // Both paths start at the root. Drop the common prefix.
        let common = from
            .iter()
            .zip(to.iter())
            .take_while(|(a, b)| a == b)
            .count();
        // Add the appropriate number of .. in front of the target.
        let mut segments: Vec<&str> = vec![".."; from.len() - common];
        segments.extend(to[common..].iter().map(|s| s.as_str()));
        Ok(segments.join("/"))
    }
}

#[async_trait(?Send)]
impl fs::server::Node for SysfsNode {
    async fn get_stats(&self) -> Result<fs::server::NodeStats, fs::server::Error> {
        let stats = match &self.kind {
            NodeKind::Directory { entries, .. } => fs::server::NodeStats {
                num_links: 2 + entries
                    .lock()
                    .expect(EXPECT_LOCK)
                    .values()
                    .filter(|child| child.is_directory())
                    .count() as u64,
                mode: 0o755,
                ..Default::default()
            },
            NodeKind::Symlink { .. } => fs::server::NodeStats {
                num_links: 1,
                mode: 0o777,
                file_size: self.symlink_target()?.len() as u64,
                ..Default::default()
            },
            NodeKind::Attribute { attr } => fs::server::NodeStats {
                num_links: 1,
                mode: if attr.writable() { 0o644 } else { 0o444 },
                file_size: attr.size(),
                ..Default::default()
            },
        };
        Ok(stats)
    }

    async fn get_link(&self, name: &str) -> Result<Option<fs::server::Child>, fs::server::Error> {
        Ok(self.lookup(name).map(|child| fs::server::Child {
            node: child.clone(),
            id: child.ino,
            file_type: child.file_type(),
        }))
    }

    async fn get_link_or_create(
        &self,
        name: &str,
        _mode: i32,
        exclusive: bool,
        _uid: i64,
        _gid: i64,
    ) -> Result<fs::server::Child, fs::server::Error> {
        // Error out on O_CREAT if the file does not exist.
        let child = self.lookup(name).ok_or(fs::server::Error::AccessDenied)?;
        if exclusive {
            return Err(fs::server::Error::AlreadyExists);
        }
        Ok(fs::server::Child {
            node: child.clone(),
            id: child.ino,
            file_type: child.file_type(),
        })
    }

    async fn open(
        &self,
        _read: bool,
        write: bool,
        _append: bool,
    ) -> Result<Arc<dyn fs::server::File>, fs::server::Error> {
        match &self.kind {
            NodeKind::Directory { entries, .. } => {
                let entries = entries
                    .lock()
                    .expect(EXPECT_LOCK)
                    .iter()
                    .map(|(name, child)| (name.clone(), child.ino as u64, child.file_type()))
                    .collect();
                Ok(Arc::new(DirectoryFile {
                    entries,
                    cursor: Mutex::new(0),
                }))
            }
            NodeKind::Attribute { attr } => {
                if write && !attr.writable() {
                    return Err(fs::server::Error::AccessDenied);
                }
                Ok(Arc::new(AttributeFile {
                    attr: attr.clone(),
                    state: async_lock::Mutex::new(AttributeState {
                        data: None,
                        offset: 0,
                    }),
                }))
            }
            // Same as posix-subsystem: do not allow symlinks to be opened.
            NodeKind::Symlink { .. } => Err(fs::server::Error::IllegalOperationTarget),
        }
    }

    async fn read_symlink(&self) -> Result<String, fs::server::Error> {
        self.symlink_target()
    }
}

/// An open directory.
// TODO: This takes a snapshot at open time.
//       We have to revisit this when we add support for seek().
//       posix-subsystem does not implement seek() for directories either.
struct DirectoryFile {
    entries: Vec<(String, u64, fs::server::FileType)>,
    cursor: Mutex<usize>,
}

#[async_trait(?Send)]
impl fs::server::File for DirectoryFile {
    async fn read_entries(&self) -> Result<Option<fs::server::DirEntry>, fs::server::Error> {
        let mut cursor = self.cursor.lock().expect(EXPECT_LOCK);
        let i = *cursor;
        if i >= self.entries.len() {
            return Ok(None);
        }
        *cursor = i + 1;
        let (name, inode, file_type) = &self.entries[i];
        Ok(Some(fs::server::DirEntry {
            name: name.clone(),
            inode: *inode,
            offset: i as i64 + 1,
            file_type: *file_type,
        }))
    }
}

/// An open attribute.
// TODO: Like posix-subsystem, this takes a snapshot on first read.
//       Linux re-runs show() on rewind and on pread() of a different offset.
struct AttributeFile {
    attr: Arc<dyn Attribute>,
    state: async_lock::Mutex<AttributeState>,
}

struct AttributeState {
    data: Option<Vec<u8>>,
    offset: usize,
}

impl AttributeFile {
    async fn read_at(
        &self,
        state: &mut AttributeState,
        offset: usize,
        buffer: &mut [u8],
    ) -> Result<usize, fs::server::Error> {
        if state.data.is_none() {
            state.data = Some(self.attr.show().await?);
        }
        let data = state.data.as_ref().unwrap();
        let offset = offset.min(data.len());
        let chunk = (data.len() - offset).min(buffer.len());
        buffer[..chunk].copy_from_slice(&data[offset..offset + chunk]);
        Ok(chunk)
    }
}

#[async_trait(?Send)]
impl fs::server::File for AttributeFile {
    async fn seek_abs(&self, offset: i64) -> Result<i64, fs::server::Error> {
        if offset < 0 {
            return Err(fs::server::Error::IllegalArgument);
        }
        self.state.lock().await.offset = offset as usize;
        Ok(offset)
    }

    async fn seek_rel(&self, offset: i64) -> Result<i64, fs::server::Error> {
        let mut state = self.state.lock().await;
        let target = state.offset as i64 + offset;
        if target < 0 {
            return Err(fs::server::Error::IllegalArgument);
        }
        state.offset = target as usize;
        Ok(target)
    }

    async fn seek_eof(&self, offset: i64) -> Result<i64, fs::server::Error> {
        let target = self.attr.size() as i64 + offset;
        if target < 0 {
            return Err(fs::server::Error::IllegalArgument);
        }
        self.state.lock().await.offset = target as usize;
        Ok(target)
    }

    async fn read(
        &self,
        _credentials: fs::server::Credentials,
        buffer: &mut [u8],
    ) -> Result<usize, fs::server::Error> {
        let mut state = self.state.lock().await;
        let offset = state.offset;
        let chunk = self.read_at(&mut state, offset, buffer).await?;
        state.offset += chunk;
        Ok(chunk)
    }

    async fn pread(
        &self,
        _credentials: fs::server::Credentials,
        offset: i64,
        buffer: &mut [u8],
    ) -> Result<usize, fs::server::Error> {
        if offset < 0 {
            return Err(fs::server::Error::IllegalArgument);
        }
        let mut state = self.state.lock().await;
        self.read_at(&mut state, offset as usize, buffer).await
    }

    async fn write(
        &self,
        _credentials: fs::server::Credentials,
        buffer: &[u8],
    ) -> Result<usize, fs::server::Error> {
        self.attr.store(buffer).await?;
        Ok(buffer.len())
    }

    async fn truncate(&self, _size: u64) -> Result<(), fs::server::Error> {
        Ok(())
    }

    async fn access_memory(&self) -> Result<Handle, fs::server::Error> {
        self.attr.access_memory().await
    }
}

/// Serves the sysfs superblock lane that posix mounts the filesystem from.
pub async fn serve_superblock(lane: Handle, root: Arc<SysfsNode>) {
    loop {
        match handle_superblock_request(&lane, &root).await {
            Ok(true) => {}
            Ok(false) => return,
            Err(e) => eprintln!("devserver: error while serving the sysfs superblock: {e:?}"),
        }
    }
}

async fn receive_tail(conversation: &Handle, tail_size: usize) -> anyhow::Result<Vec<u8>> {
    let mut tail = vec![0u8; tail_size];
    hel::submit_async(conversation, hel::ReceiveBuffer::new(&mut tail)).await??;
    Ok(tail)
}

async fn handle_superblock_request(lane: &Handle, root: &Arc<SysfsNode>) -> anyhow::Result<bool> {
    let (conv, (head,)) = hel::submit_async(lane, hel::Accept::new((hel::ReceiveInline,))).await?;

    let conversation = match conv {
        Ok(Some(lane)) => lane,
        Ok(None) => anyhow::bail!("accept did not yield a conversation lane"),
        Err(hel::Error::EndOfLane) | Err(hel::Error::LaneShutdown) => return Ok(false),
        Err(e) => return Err(e.into()),
    };
    let head = head?;

    let preamble = bragi::preamble_from_bytes(&head)?;
    match preamble.id() {
        fs_proto::MountRequest::MESSAGE_ID => {
            let tail = receive_tail(&conversation, preamble.tail_size() as usize).await?;
            let _req: fs_proto::MountRequest = bragi::head_tail_from_bytes(&head, &tail)?;

            let (local, remote) = hel::create_stream()?;
            fs::server::serve_node(local, root.clone());

            let resp =
                fs_proto::MountResponse::new(fs_proto::Errors::Success, 0, ROOT_INODE as u64);
            let resp_head = bragi::head_to_bytes(&resp)?;
            let (send_resp, push_node) = hel::submit_async(
                &conversation,
                (
                    hel::SendBuffer::new(&resp_head),
                    hel::PushDescriptor::new(
                        &remote,
                        hel_sys::kHelRightInvoke | hel_sys::kHelRightManage,
                    ),
                ),
            )
            .await?;
            send_resp?;
            push_node?;
        }
        fs_proto::GetFsStatsRequest::MESSAGE_ID => {
            let resp = fs_proto::GetFsStatsResponse::new(
                fs_proto::Errors::Success,
                SYSFS_MAGIC,
                4096,
                4096,
                0,
                0,
                0,
                0,
                0,
                0,
                255,
                0,
                0,
                0,
            );
            let resp_head = bragi::head_to_bytes(&resp)?;
            hel::submit_async(&conversation, hel::SendBuffer::new(&resp_head)).await??;
        }
        id => {
            eprintln!("devserver: dismissing superblock request with unexpected message ID {id}");
            hel::submit_async(&conversation, hel::Dismiss).await??;
        }
    }
    Ok(true)
}
