//! Flattened device tree parser; port of the raw parser in thor's kernel/common/dtb.hpp.

use zerocopy::FromBytes;
use zerocopy::byteorder::big_endian::U32;

/// Points to an array of u32 cells (usually within a DT property).
///
/// For example, this can be used to point to #address-cells-many cells.
#[derive(Clone, Copy)]
pub struct Cells<'a> {
    p: &'a [u8],
}

impl<'a> Cells<'a> {
    fn new(p: &'a [u8]) -> Cells<'a> {
        assert!(p.len() % size_of::<u32>() == 0);
        Cells { p }
    }

    pub fn num_cells(&self) -> usize {
        self.p.len() / size_of::<u32>()
    }

    pub fn read(&self) -> Option<u64> {
        // Fail if a u64 cannot hold enough data.
        if size_of::<u64>() < size_of::<u32>() * self.num_cells() {
            return None;
        }

        let mut v: u64 = 0;
        for i in 0..self.num_cells() {
            let s =
                U32::read_from_bytes(&self.p[size_of::<u32>() * i..][..size_of::<u32>()]).unwrap();
            v |= u64::from(s.get()) << ((self.num_cells() - i - 1) * 32);
        }
        Some(v)
    }

    pub fn into_slice(&self, offset: usize, num_cells: usize) -> Option<Cells<'a>> {
        // Fail if out-of-bounds.
        if offset + num_cells > self.num_cells() {
            return None;
        }
        Some(Cells::new(
            &self.p[size_of::<u32>() * offset..][..size_of::<u32>() * num_cells],
        ))
    }

    pub fn read_slice(&self, offset: usize, num_cells: usize) -> Option<u64> {
        self.into_slice(offset, num_cells)?.read()
    }
}

/// Points to an offset within a DT property.
#[derive(Clone, Copy)]
pub struct Accessor<'a> {
    data: &'a [u8],
    offset: usize,
}

impl<'a> Accessor<'a> {
    fn new(data: &'a [u8]) -> Accessor<'a> {
        Accessor { data, offset: 0 }
    }

    pub fn at_end_of_property(&self) -> bool {
        assert!(self.offset <= self.data.len());
        self.offset == self.data.len()
    }

    pub fn advance(&mut self, num_bytes: usize) {
        assert!(self.offset + num_bytes <= self.data.len());
        self.offset += num_bytes;
    }

    pub fn into_cells(&self, num_cells: usize) -> Option<Cells<'a>> {
        // Fail if out-of-bounds.
        if self.offset + size_of::<u32>() * num_cells > self.data.len() {
            return None;
        }
        Some(Cells::new(
            &self.data[self.offset..][..size_of::<u32>() * num_cells],
        ))
    }

    pub fn read_cells(&self, num_cells: usize) -> Option<u64> {
        self.into_cells(num_cells)?.read()
    }
}

#[derive(FromBytes)]
#[repr(C)]
struct DtbHeader {
    magic: U32,
    totalsize: U32,
    off_dt_struct: U32,
    off_dt_strings: U32,
    off_mem_rsvmap: U32,
    version: U32,
    last_comp_version: U32,
    boot_cpuid_phys: U32,
    size_dt_strings: U32,
    size_dt_struct: U32,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Tag {
    BeginNode = 1,
    EndNode = 2,
    Prop = 3,
    End = 9,
}

pub trait DeviceTreeWalker<'a> {
    fn push(&mut self, node: DeviceTreeNode<'a>);
    fn pop(&mut self);
}

pub struct DeviceTree<'a> {
    data: &'a [u8],

    strings_block: usize,
    structure_block: usize,
}

impl<'a> DeviceTree<'a> {
    pub fn new(data: &'a [u8]) -> DeviceTree<'a> {
        let header = DtbHeader::read_from_prefix(data).unwrap().0;
        assert!(header.magic.get() == 0xd00dfeed);

        DeviceTree {
            data,
            strings_block: header.off_dt_strings.get() as usize,
            structure_block: header.off_dt_struct.get() as usize,
        }
    }

    pub fn root_node(&'a self) -> DeviceTreeNode<'a> {
        DeviceTreeNode::new(self, self.structure_block)
    }

    fn string(&self, offset: usize) -> &'a str {
        read_string_at(self.data, self.strings_block + offset)
    }
}

#[derive(Clone, Copy)]
pub struct DeviceTreeProperty<'a> {
    name: &'a str,
    data: &'a [u8],
}

impl<'a> DeviceTreeProperty<'a> {
    pub fn name(&self) -> &'a str {
        self.name
    }

    pub fn data(&self) -> &'a [u8] {
        self.data
    }

    pub fn size(&self) -> usize {
        self.data.len()
    }

    pub fn access(&self) -> Accessor<'a> {
        Accessor::new(self.data)
    }

    pub fn as_u32(&self, offset: usize) -> u32 {
        U32::read_from_bytes(&self.data[offset..][..size_of::<u32>()])
            .unwrap()
            .get()
    }

    pub fn as_u64(&self, offset: usize) -> u64 {
        (u64::from(self.as_u32(offset)) << 32) | u64::from(self.as_u32(offset + 4))
    }

    pub fn as_prop_array_entry(&self, n_cells: usize, offset: usize) -> u64 {
        match n_cells {
            0 => 0,
            1 => self.as_u32(offset).into(),
            2 => self.as_u64(offset),
            _ => panic!("Invalid amount of cells"),
        }
    }
}

// Reads a NUL-terminated string at the given offset.
pub(crate) fn read_string_at(data: &[u8], offset: usize) -> &str {
    let bytes = &data[offset..];
    let len = bytes
        .iter()
        .position(|&b| b == 0)
        .expect("sif: unterminated string in DTB");
    str::from_utf8(&bytes[..len]).expect("sif: DTB string is not valid UTF-8")
}

fn read_tag(data: &[u8], ptr: &mut usize) -> Tag {
    loop {
        let raw = U32::read_from_bytes(&data[*ptr..][..size_of::<u32>()])
            .unwrap()
            .get();
        *ptr += 4;
        let t = match raw {
            1 => Tag::BeginNode,
            2 => Tag::EndNode,
            3 => Tag::Prop,
            4 => continue, // Skip nop tags.
            9 => Tag::End,
            _ => panic!("Unknown tag {raw}"),
        };

        assert!(t != Tag::End);
        return t;
    }
}

fn read_string_inline<'a>(data: &'a [u8], ptr: &mut usize) -> &'a str {
    let s = read_string_at(data, *ptr);
    *ptr += (s.len() + 4) & !3;
    s
}

fn read_length(data: &[u8], ptr: &mut usize) -> usize {
    let len = U32::read_from_bytes(&data[*ptr..][..size_of::<u32>()])
        .unwrap()
        .get();
    *ptr += 4;
    len as usize
}

fn skip_prop(data: &[u8], ptr: &mut usize) {
    let len = read_length(data, ptr);
    *ptr += 4; // Skip the name.
    *ptr += (len + 3) & !3; // Skip the data.
}

#[derive(Clone, Copy)]
pub struct DeviceTreeNode<'a> {
    tree: &'a DeviceTree<'a>,

    node_off: usize,
    prop_off: usize,
    name: &'a str,
}

impl<'a> DeviceTreeNode<'a> {
    fn new(tree: &'a DeviceTree<'a>, base: usize) -> DeviceTreeNode<'a> {
        let mut tmp = base;

        let tag = read_tag(tree.data, &mut tmp);
        assert!(tag == Tag::BeginNode);
        let name = read_string_inline(tree.data, &mut tmp);

        let prop_off = tmp;
        let node_off = Self::find_node_off(tree.data, prop_off);

        DeviceTreeNode {
            tree,
            node_off,
            prop_off,
            name,
        }
    }

    fn find_node_off(data: &[u8], prop_off: usize) -> usize {
        let mut ptr = prop_off;

        loop {
            let tag = read_tag(data, &mut ptr);

            if tag != Tag::Prop {
                return ptr - 4; // Pointer to the tag.
            }

            skip_prop(data, &mut ptr);
        }
    }

    pub fn name(&self) -> &'a str {
        self.name
    }

    pub fn walk_children(&self, walker: &mut impl DeviceTreeWalker<'a>) {
        let data = self.tree.data;
        let mut ptr = self.node_off;
        let mut depth = 0;

        loop {
            let tag = read_tag(data, &mut ptr);

            match tag {
                Tag::BeginNode => {
                    // Construct a node and push it.
                    depth += 1;
                    walker.push(DeviceTreeNode::new(self.tree, ptr - 4));
                    read_string_inline(data, &mut ptr);
                }
                Tag::Prop => {
                    // Skip properties of subnodes.
                    skip_prop(data, &mut ptr);
                }
                Tag::EndNode => {
                    // Pop a node.
                    walker.pop();
                    if depth == 0 {
                        return;
                    }
                    depth -= 1;
                }
                Tag::End => unreachable!(),
            }
        }
    }

    pub fn find_property(&self, name: &str) -> Option<DeviceTreeProperty<'a>> {
        self.properties().find(|prop| prop.name() == name)
    }

    pub fn properties(&self) -> PropertyIter<'a> {
        PropertyIter {
            tree: self.tree,
            ptr: self.prop_off,
            end: self.node_off,
        }
    }
}

pub struct PropertyIter<'a> {
    tree: &'a DeviceTree<'a>,
    ptr: usize,
    end: usize,
}

impl<'a> Iterator for PropertyIter<'a> {
    type Item = DeviceTreeProperty<'a>;

    fn next(&mut self) -> Option<DeviceTreeProperty<'a>> {
        if self.ptr == self.end {
            return None;
        }

        let data = self.tree.data;
        let mut p = self.ptr;

        let tag = read_tag(data, &mut p);
        assert!(tag == Tag::Prop);

        let len = read_length(data, &mut p);
        let name_off = read_length(data, &mut p);
        let name = self.tree.string(name_off);
        let prop_data = &data[p..][..len];
        p += (len + 3) & !3;

        // Skip potential nop tags before the next property.
        read_tag(data, &mut p);
        self.ptr = p - 4; // Rewind back to the tag.

        Some(DeviceTreeProperty {
            name,
            data: prop_data,
        })
    }
}
