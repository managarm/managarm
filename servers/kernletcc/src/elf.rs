//! Minimal ELF64 DSO emitter for kernlets, producing exactly the format the kernel loader accepts,
//! i.e., restricted set of dynamic tags, a SysV hash table and JUMP_SLOT relocations only.

use anyhow::{Result, ensure};
use object::Endianness;
use object::elf;
use object::write::elf::{FileHeader, ProgramHeader, Rel, Sym, SymbolIndex, Writer};

use crate::fafnir::{Arch, Compiled, GotReloc, GotRelocKind};

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

/// The ELF machine and JUMP_SLOT relocation type of an architecture.
/// Keep in sync with kernletMachine/kernletJumpSlot in kernel/thor/generic/kernlet.cpp.
fn elf_arch(arch: Arch) -> (u16, u32) {
    match arch {
        Arch::X86_64 => (elf::EM_X86_64, elf::R_X86_64_JUMP_SLOT),
        Arch::Aarch64 => (elf::EM_AARCH64, elf::R_AARCH64_JUMP_SLOT),
        Arch::Riscv64 => (elf::EM_RISCV, elf::R_RISCV_JUMP_SLOT),
    }
}

/// Rewrites the 4-byte instruction at `offset` in place.
fn patch_insn(code: &mut [u8], offset: usize, f: impl FnOnce(u32) -> u32) {
    let field = &mut code[offset..offset + 4];
    let insn = u32::from_le_bytes(field.try_into().unwrap());
    field.copy_from_slice(&f(insn).to_le_bytes());
}

/// Encodes the address of a GOT slot into the instruction field that references it.
/// `got_vaddr` is the address of the slot, `text_off` the address of the code.
fn patch_got_reference(
    code: &mut [u8],
    reloc: &GotReloc,
    got_vaddr: usize,
    text_off: usize,
) -> Result<()> {
    let slot = got_vaddr as i64 + reloc.addend;
    let pc = (text_off + reloc.pc_offset as usize) as i64;
    let field = reloc.offset as usize;

    match reloc.kind {
        GotRelocKind::X86Pcrel32 => {
            let value = i32::try_from(slot - pc)
                .map_err(|_| anyhow::anyhow!("GOT displacement out of range"))?;
            code[field..field + 4].copy_from_slice(&value.to_le_bytes());
        }
        GotRelocKind::Aarch64AdrpPage => {
            // The adrp immediate counts 4 KiB pages and is split into immlo and immhi.
            let pages = (slot >> 12) - (pc >> 12);
            ensure!(
                (-(1 << 20)..(1 << 20)).contains(&pages),
                "GOT page offset out of range"
            );
            let imm = pages as u32;
            patch_insn(code, field, |insn| {
                (insn & !((0x3 << 29) | (0x7FFFF << 5)))
                    | ((imm & 0x3) << 29)
                    | (((imm >> 2) & 0x7FFFF) << 5)
            });
        }
        GotRelocKind::Aarch64LdrLo12 => {
            // The 64-bit ldr immediate is scaled by the access size.
            ensure!(slot % 8 == 0, "GOT slot is not 8-byte aligned");
            let imm = ((slot as u32) & 0xFFF) >> 3;
            patch_insn(code, field, |insn| (insn & !(0xFFF << 10)) | (imm << 10));
        }
        GotRelocKind::RiscvAuipcHi20 => {
            // The auipc immediate is rounded such that the (sign-extended) low 12 bits of the
            // paired ld cancel the rounding out again.
            let offset = i32::try_from(slot - pc)
                .map_err(|_| anyhow::anyhow!("GOT displacement out of range"))?;
            let hi20 = ((offset as u32).wrapping_add(0x800) >> 12) & 0xFFFFF;
            patch_insn(code, field, |insn| (insn & 0xFFF) | (hi20 << 12));
        }
        GotRelocKind::RiscvLoadLo12 => {
            let offset = i32::try_from(slot - pc)
                .map_err(|_| anyhow::anyhow!("GOT displacement out of range"))?;
            let lo12 = (offset as u32) & 0xFFF;
            patch_insn(code, field, |insn| (insn & 0x000F_FFFF) | (lo12 << 20));
        }
    }
    Ok(())
}

/// Builds an ELF DSO from compiled kernlet code and its external relocations.
pub fn build_dso(compiled: &Compiled) -> Result<Vec<u8>> {
    let (machine, jump_slot) = elf_arch(compiled.arch);

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
        e_machine: machine,
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

    // Code (.text); patch each GOT reference to point at its GOT slot.
    let mut code = compiled.code.clone();
    for reloc in &compiled.relocs {
        let got_vaddr = got_off + reloc.symbol as usize * 8;
        patch_got_reference(&mut code, reloc, got_vaddr, text_off)?;
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
                r_type: jump_slot,
                r_addend: 0,
            },
        );
    }
    w.write_align(8); // Matches the trailing `reserve(0, 8)` above.

    ensure!(w.len() == total, "internal layout mismatch");
    drop(w);
    Ok(buffer)
}
