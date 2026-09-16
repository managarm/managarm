//! Implements the sysfs file system.

use std::collections::BTreeMap;
use std::sync::{Arc, Mutex, Weak};

use async_trait::async_trait;
use hel::Handle;
use managarm::fs;

use crate::EXPECT_LOCK;

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

/// (parent, name) pair of a node in sysfs.
#[derive(Default)]
struct Location {
    parent: Weak<SysfsNode>,
    name: String,
}

pub struct SysfsNode {
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
    fn new(kind: NodeKind) -> Arc<Self> {
        Arc::new_cyclic(|self_ref| Self {
            self_ref: self_ref.clone(),
            location: Mutex::new(Location::default()),
            kind,
        })
    }

    fn new_directory() -> Arc<Self> {
        Self::new(NodeKind::Directory {
            entries: Mutex::new(BTreeMap::new()),
        })
    }

    pub fn new_root() -> Arc<Self> {
        Self::new_directory()
    }

    pub fn is_directory(&self) -> bool {
        matches!(self.kind, NodeKind::Directory { .. })
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

    /// Creates a directory or fails if it already exists.
    pub fn create_dir(self: &Arc<Self>, name: &str) -> Result<Arc<SysfsNode>, TreeError> {
        let node = Self::new_directory();
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
        let node = Self::new_directory();
        self.attach(&mut entries, name, node.clone());
        Ok(node)
    }

    pub fn create_link(
        self: &Arc<Self>,
        name: &str,
        target: &Arc<SysfsNode>,
    ) -> Result<(), TreeError> {
        let node = Self::new(NodeKind::Symlink {
            target: target.self_ref.clone(),
        });
        self.insert(name, node)
    }

    pub fn create_attr(
        self: &Arc<Self>,
        name: &str,
        attr: Arc<dyn Attribute>,
    ) -> Result<(), TreeError> {
        let node = Self::new(NodeKind::Attribute { attr });
        self.insert(name, node)
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
}
