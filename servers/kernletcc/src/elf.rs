//! Minimal ELF64 DSO emitter for kernlets, producing exactly the format the kernel loader accepts,
//! i.e., restricted set of dynamic tags, a SysV hash table and JUMP_SLOT relocations only.

use anyhow::{Result, ensure};
use object::Endianness;
use object::elf;
use object::write::elf::{FileHeader, ProgramHeader, Rel, Sym, SymbolIndex, Writer};

use crate::fafnir::Compiled;

const SYM_SIZE: usize = size_of::<elf::Sym64<Endianness>>();
const RELA_SIZE: usize = size_of::<elf::Rela64<Endianness>>();
const DYN_SIZE: usize = size_of::<elf::Dyn64<Endianness>>();

fn sysv_elf_hash(name: &str) -> u32 {
    let mut h: u32 = 0;
    for b in name.bytes() {
        h = h.wrapping_shl(4).wrapping_add(b as u32);
        let g = h & 0xF000_0000;
        if g != 0 {
            h ^= g >> 24;
        }
        h &= 0x0FFF_FFFF;
    }
    h
}

/// Builds an ELF DSO from compiled kernlet code and its external relocations.
pub fn build_dso(compiled: &Compiled) -> Result<Vec<u8>> {
    let mut buffer = Vec::new();
    let mut w = Writer::new(Endianness::Little, true, &mut buffer);

    // Reserve dynamic-symbol indices: exports first, then externals.
    // An external's index in `compiled.externs` doubles as its GOT slot.
    for export in &compiled.exports {
        w.add_dynamic_string(export.name.as_bytes());
        w.reserve_dynamic_symbol_index();
    }
    let extern_syms: Vec<SymbolIndex> = compiled
        .externs
        .iter()
        .map(|name| {
            w.add_dynamic_string(name.as_bytes());
            w.reserve_dynamic_symbol_index()
        })
        .collect();

    // Ordered symbol names for the hash table: exports then externals.
    let sym_names: Vec<&str> = compiled
        .exports
        .iter()
        .map(|e| e.name.as_str())
        .chain(compiled.externs.iter().map(|s| s.as_str()))
        .collect();

    // RESERVE PHASE: Reserve file offsets (depends only on the symbol/relocation counts).
    w.reserve_file_header();
    w.reserve_program_headers(2);

    let text_off = w.reserve(compiled.code.len(), 8);
    let got_size = compiled.externs.len() * 8;
    let got_off = w.reserve(got_size, 8);

    let dynstr_off = w.reserve_dynstr();
    let dynsym_off = w.reserve_dynsym();
    let nsyms = w.dynamic_symbol_count();

    // Hash table over all symbols.
    let nbuckets = nsyms.max(1);
    let hash_off = w.reserve_hash(nbuckets, nsyms);

    let dyn_off = w.reserve_dynamic(7);
    let dyn_size = 7 * DYN_SIZE;

    // One JUMP_SLOT relocation per GOT slot.
    let rela_off = w.reserve_relocations(compiled.externs.len(), true);
    let rela_size = compiled.externs.len() * RELA_SIZE;

    w.reserve(0, 8); // Round the reserved total up to an 8-byte boundary.
    let total = w.reserved_len();

    // WRITE PHASE: same calls in the same order as the reserve phase.
    // `check_off` asserts each part lands at its reserved offset.
    let check_off = |pos: usize, reserved: usize, section: &str| -> Result<()> {
        ensure!(
            pos == reserved,
            "{section}: writing at {pos:#x}, reserved {reserved:#x}"
        );
        Ok(())
    };
    w.write_file_header(&FileHeader {
        os_abi: elf::ELFOSABI_NONE,
        abi_version: 0,
        e_type: elf::ET_DYN,
        e_machine: elf::EM_X86_64,
        e_entry: 0,
        e_flags: 0,
    })?;

    w.write_align_program_headers();
    // One PT_LOAD covering the whole image.
    w.write_program_header(&ProgramHeader {
        p_type: elf::PT_LOAD,
        p_flags: elf::PF_R | elf::PF_X,
        p_offset: 0,
        p_vaddr: 0,
        p_paddr: 0,
        p_filesz: total as u64,
        p_memsz: total as u64,
        p_align: 0x1000,
    });
    w.write_program_header(&ProgramHeader {
        p_type: elf::PT_DYNAMIC,
        p_flags: elf::PF_R,
        p_offset: dyn_off as u64,
        p_vaddr: dyn_off as u64,
        p_paddr: 0,
        p_filesz: dyn_size as u64,
        p_memsz: dyn_size as u64,
        p_align: 8,
    });

    // Code (.text); patch each GOT-relative field to its GOT slot via S + A - P.
    let mut code = compiled.code.clone();
    for reloc in &compiled.relocs {
        let got_vaddr = (got_off + reloc.symbol as usize * 8) as i64;
        let field_vaddr = (text_off + reloc.offset as usize) as i64;
        let value = got_vaddr + reloc.addend - field_vaddr;
        let value =
            i32::try_from(value).map_err(|_| anyhow::anyhow!("GOT displacement out of range"))?;
        code[reloc.offset as usize..reloc.offset as usize + 4]
            .copy_from_slice(&value.to_le_bytes());
    }
    w.write_align(8);
    check_off(w.len(), text_off, ".text")?;
    w.write(&code);

    // GOT: initialized to zero. The kernel writes resolved addresses via JUMP_SLOT relocations.
    w.write_align(8);
    check_off(w.len(), got_off, ".got")?;
    w.write(&vec![0u8; got_size]);

    // String table.
    check_off(w.len(), dynstr_off, ".dynstr")?;
    w.write_dynstr();

    // Symbol table, in order reserved above: null symbol, exports, externals.
    w.write_align(8);
    check_off(w.len(), dynsym_off, ".dynsym")?;
    w.write_null_dynamic_symbol();
    for export in &compiled.exports {
        let name = w.get_dynamic_string(export.name.as_bytes());
        w.write_dynamic_symbol(&Sym {
            name: Some(name),
            section: None,
            st_info: (elf::STB_GLOBAL << 4) | elf::STT_FUNC,
            st_other: 0,
            // any non-SHN_UNDEF section; the loader only checks for SHN_UNDEF
            st_shndx: 1,
            st_value: (text_off + export.offset as usize) as u64,
            st_size: export.size as u64,
        });
    }
    for ext_name in &compiled.externs {
        let name = w.get_dynamic_string(ext_name.as_bytes());
        w.write_dynamic_symbol(&Sym {
            name: Some(name),
            section: None,
            st_info: (elf::STB_GLOBAL << 4) | elf::STT_FUNC,
            st_other: 0,
            st_shndx: elf::SHN_UNDEF,
            st_value: 0,
            st_size: 0,
        });
    }

    // Hash table: [nbuckets, nchains, buckets..., chain...].
    check_off(w.len(), hash_off, ".hash")?;
    w.write_hash(nbuckets, nsyms, |i| {
        // The null symbol (written by write_null_dynamic_symbol()) does not map to any hash.
        if i == 0 {
            None
        } else {
            Some(sysv_elf_hash(sym_names[(i - 1) as usize]))
        }
    });

    // Dynamic section. Only tags accepted by the kernel loader are emitted.
    w.write_align_dynamic();
    check_off(w.len(), dyn_off, ".dynamic")?;
    w.write_dynamic(elf::DT_STRTAB, dynstr_off as u64);
    w.write_dynamic(elf::DT_SYMTAB, dynsym_off as u64);
    w.write_dynamic(elf::DT_SYMENT, SYM_SIZE as u64);
    w.write_dynamic(elf::DT_HASH, hash_off as u64);
    w.write_dynamic(elf::DT_JMPREL, rela_off as u64);
    w.write_dynamic(elf::DT_PLTRELSZ, rela_size as u64);
    w.write_dynamic(elf::DT_NULL, 0);

    // Relocations (.rela.plt): one JUMP_SLOT per external symbol.
    w.write_align_relocation();
    check_off(w.len(), rela_off, ".rela.plt")?;
    for (i, symidx) in extern_syms.iter().enumerate() {
        w.write_relocation(
            true,
            &Rel {
                r_offset: (got_off + i * 8) as u64,
                r_sym: symidx.0,
                r_type: elf::R_X86_64_JUMP_SLOT,
                r_addend: 0,
            },
        );
    }
    w.write_align(8); // Matches the trailing `reserve(0, 8)` above.

    ensure!(w.len() == total, "internal layout mismatch");
    drop(w);
    Ok(buffer)
}
