//! Translation of Fafnir IR bytecode into machine code via Cranelift.
//!
//! The bytecode is untrusted input from userspace drivers.
//! The compiler must never panic on invalid IR.

use anyhow::{Result, bail, ensure};
use cranelift_codegen::binemit::Reloc;
use cranelift_codegen::ir::{
    AbiParam, ExtFuncData, ExternalName, Function, InstBuilder, MemFlags, Signature, Type,
    UserExternalName, UserFuncName, Value, types,
};
use cranelift_codegen::isa::CallConv;
use cranelift_codegen::settings::{self, Configurable};
use cranelift_codegen::{Context, FinalizedRelocTarget};
use cranelift_control::ControlPlane;
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Variable};
use indexmap::IndexSet;

// Fafnir opcodes, in the order defined by fafnir/language.h.
const FNR_OP_NULL: u8 = 0;
const FNR_OP_DROP: u8 = 1;
const FNR_OP_DUP: u8 = 2;
const FNR_OP_BINDING: u8 = 3;
const FNR_OP_S_VALUE: u8 = 5;
const FNR_OP_CHECK_IF: u8 = 6;
const FNR_OP_THEN: u8 = 7;
const FNR_OP_ELSE_THEN: u8 = 8;
const FNR_OP_END: u8 = 9;
const FNR_OP_LITERAL: u8 = 10;
const FNR_OP_BITWISE_AND: u8 = 11;
const FNR_OP_ADD: u8 = 12;
const FNR_OP_INTRIN: u8 = 13;
const FNR_OP_SCOPE: u8 = 14;
const FNR_OP_SCOPE_END: u8 = 15;

/// The kind of value a kernlet binding parameter holds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BindType {
    Offset,
    MemoryView,
    BitsetEvent,
}

/// A relocation referencing an external (kernel-provided) symbol.
#[derive(Debug, Clone)]
pub struct ExternReloc {
    /// Offset of the 4-byte PC-relative field within the code buffer.
    pub offset: u32,
    /// Addend supplied by Cranelift.
    pub addend: i64,
    /// Index into `Compiled::externs`.
    pub symbol: u32,
}

/// A symbol exported (defined) by the kernlet, located at `offset` within `code`.
#[derive(Debug, Clone)]
pub struct Export {
    pub name: String,
    pub offset: u32,
    pub size: u32,
}

/// The result of compiling a kernlet: machine code, the symbols it exports
/// and its external relocations.
pub struct Compiled {
    pub code: Vec<u8>,
    /// Exported symbols.
    pub exports: Vec<Export>,
    /// External symbols. `ExternReloc::symbol` indexes into this vector.
    pub externs: Vec<String>,
    pub relocs: Vec<ExternReloc>,
}

struct Reader<'a> {
    code: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn new(code: &'a [u8]) -> Self {
        Self { code, pos: 0 }
    }

    fn eof(&self) -> bool {
        self.pos >= self.code.len()
    }

    /// Reads a single-byte unsigned operand.
    fn read_uint(&mut self) -> Result<u8> {
        let b = *self
            .code
            .get(self.pos)
            .ok_or_else(|| anyhow::anyhow!("unexpected end of bytecode"))?;
        self.pos += 1;
        Ok(b)
    }

    /// Reads a null-terminated string operand.
    fn read_string(&mut self) -> Result<String> {
        let mut s = String::new();
        loop {
            let c = self.read_uint()?;
            if c == 0 {
                break;
            }
            s.push(c as char);
        }
        Ok(s)
    }
}

/// A block and its operand stack (opstack).
struct Block {
    kind: BlockKind,
    opstack: Vec<cranelift_codegen::ir::Value>,
}

impl Block {
    fn new(kind: BlockKind) -> Self {
        Self {
            kind,
            opstack: Vec::new(),
        }
    }
}

enum BlockKind {
    /// The outermost block.
    Root,
    /// A block opened by SCOPE, paired with an sstack on the scope stack.
    Scope,
    /// The condition block opened by CHECK_IF, before THEN.
    IfCond,
    /// The 'then' block; carries the branch target for the else-part.
    IfThen {
        else_block: cranelift_codegen::ir::Block,
    },
    /// The 'else' block; carries the merge target and one merge variable per branch result.
    IfElse {
        merge_block: cranelift_codegen::ir::Block,
        merge_vars: Vec<Variable>,
    },
}

/// Signatures of the kernel intrinsics from `resolveExternal` (kernel/thor/generic/kernlet.cpp):
/// parameter types plus i32 return count.
/// Keep in sync with thor. Reject unknown names.
fn intrinsic_signature(name: &str) -> Option<(&'static [Type], usize)> {
    use types::{I32, I64};
    match name {
        "__pio_read16" => Some((&[I32], 1)),
        "__pio_write16" => Some((&[I32, I32], 0)),
        "__mmio_read8" => Some((&[I64, I32], 1)),
        "__mmio_read32" => Some((&[I64, I32], 1)),
        "__mmio_write32" => Some((&[I64, I32, I32], 0)),
        "__trigger_bitset" => Some((&[I64, I32], 0)),
        _ => None,
    }
}

/// The block and scope stacks maintained during compilation.
struct Compiler {
    blocks: Vec<Block>,
    scopes: Vec<Vec<Value>>,
}

impl Compiler {
    fn new() -> Self {
        // The outermost block and scope are implicit.
        Self {
            blocks: vec![Block::new(BlockKind::Root)],
            scopes: vec![Vec::new()],
        }
    }

    /// The innermost open block.
    fn current_block_mut(&mut self) -> Result<&mut Block> {
        self.blocks
            .last_mut()
            .ok_or_else(|| anyhow::anyhow!("operation with no open block"))
    }

    /// The innermost open block's opstack.
    fn opstack_mut(&mut self) -> Result<&mut Vec<Value>> {
        Ok(&mut self.current_block_mut()?.opstack)
    }

    /// The innermost open scope's sstack.
    fn current_sstack(&self) -> Result<&Vec<Value>> {
        self.scopes
            .last()
            .ok_or_else(|| anyhow::anyhow!("operation with no open scope"))
    }

    fn open_block(&mut self, kind: BlockKind) {
        self.blocks.push(Block::new(kind));
    }

    fn close_block(&mut self) -> Result<Block> {
        self.blocks
            .pop()
            .ok_or_else(|| anyhow::anyhow!("operation with no open block"))
    }

    /// Closes the current block and opens a fresh one in its place (THEN, ELSE_THEN).
    fn replace_block(&mut self, kind: BlockKind) -> Result<()> {
        self.close_block()?;
        self.blocks.push(Block::new(kind));
        Ok(())
    }

    fn open_scope(&mut self, sstack: Vec<Value>) {
        self.scopes.push(sstack);
    }

    fn close_scope(&mut self) {
        self.scopes.pop();
    }

    /// Checks that only the outermost block remains and returns the kernlet's single result.
    fn return_value(&mut self) -> Result<Value> {
        ensure!(
            self.blocks.len() == 1 && matches!(self.blocks[0].kind, BlockKind::Root),
            "unterminated block (missing END or SCOPE_END)"
        );
        let opstack = &mut self.blocks[0].opstack;
        ensure!(
            opstack.len() == 1,
            "kernlet must leave exactly one return value"
        );
        Ok(opstack.pop().unwrap())
    }
}

/// Compiles Fafnir bytecode into x86-64 machine code.
pub fn compile(code: &[u8], bind_types: &[BindType]) -> Result<Compiled> {
    // Generate PIC code: Cranelift emits X86GOTPCRel4 relocations for external symbols
    // (GOT-relative loads) that we later resolve to GOT slots the kernel fills via JUMP_SLOT.
    let mut flags = settings::builder();
    flags.set("opt_level", "speed").unwrap();
    flags.set("is_pic", "true").unwrap();
    let triple: target_lexicon::Triple = "x86_64-unknown-none-elf".parse().unwrap();
    let isa = cranelift_codegen::isa::lookup(triple)?.finish(settings::Flags::new(flags))?;

    // Signature of the entry point: automate_irq(const void *instance) -> int
    let mut sig = Signature::new(CallConv::SystemV);
    sig.params.push(AbiParam::new(types::I64));
    sig.returns.push(AbiParam::new(types::I32));
    let mut func = Function::with_name_signature(UserFuncName::user(0, 0), sig);

    let mut fbctx = FunctionBuilderContext::new();
    let mut builder = FunctionBuilder::new(&mut func, &mut fbctx);

    // Each binding occupies an 8-byte slot in the instance struct.
    // This needs to match the kernel.
    let bindings: Vec<(BindType, i32)> = bind_types
        .iter()
        .enumerate()
        .map(|(i, &bt)| (bt, (i * 8) as i32))
        .collect();

    let entry = builder.create_block();
    builder.append_block_params_for_function_params(entry);
    builder.switch_to_block(entry);
    let instance = builder.block_params(entry)[0];

    // De-duplicated external symbols referenced by INTRIN calls.
    let mut externs: IndexSet<String> = IndexSet::new();

    let mut compiler = Compiler::new();

    let mut r = Reader::new(code);
    while !r.eof() {
        let opcode = r.read_uint()?;
        match opcode {
            FNR_OP_NULL => {}
            FNR_OP_DROP => {
                let opstack = compiler.opstack_mut()?;
                opstack
                    .pop()
                    .ok_or_else(|| anyhow::anyhow!("DROP on empty opstack"))?;
            }
            FNR_OP_DUP => {
                let index = r.read_uint()? as usize;
                let opstack = compiler.opstack_mut()?;
                ensure!(opstack.len() > index, "DUP index out of range");
                let v = opstack[opstack.len() - index - 1];
                opstack.push(v);
            }
            FNR_OP_LITERAL => {
                let value = r.read_uint()? as i64;
                let v = builder.ins().iconst(types::I32, value);
                compiler.opstack_mut()?.push(v);
            }
            FNR_OP_BINDING => {
                let index = r.read_uint()? as usize;
                let (bt, disp) = *bindings
                    .get(index)
                    .ok_or_else(|| anyhow::anyhow!("binding index out of range"))?;
                let ty = match bt {
                    BindType::Offset => types::I32,
                    BindType::MemoryView | BindType::BitsetEvent => types::I64,
                };
                let v = builder.ins().load(ty, MemFlags::trusted(), instance, disp);
                compiler.opstack_mut()?.push(v);
            }
            FNR_OP_S_VALUE => {
                let index = r.read_uint()? as usize;
                let v = *compiler
                    .current_sstack()?
                    .get(index)
                    .ok_or_else(|| anyhow::anyhow!("S_VALUE index out of range"))?;
                compiler.opstack_mut()?.push(v);
            }
            FNR_OP_BITWISE_AND => {
                let opstack = compiler.opstack_mut()?;
                ensure!(opstack.len() >= 2, "BITWISE_AND needs two operands");
                let right = opstack.pop().unwrap();
                let left = opstack.pop().unwrap();
                ensure!(
                    builder.func.dfg.value_type(left) == builder.func.dfg.value_type(right),
                    "BITWISE_AND operands must have the same width"
                );
                opstack.push(builder.ins().band(left, right));
            }
            FNR_OP_ADD => {
                let opstack = compiler.opstack_mut()?;
                ensure!(opstack.len() >= 2, "ADD needs two operands");
                let right = opstack.pop().unwrap();
                let left = opstack.pop().unwrap();
                ensure!(
                    builder.func.dfg.value_type(left) == builder.func.dfg.value_type(right),
                    "ADD operands must have the same width"
                );
                opstack.push(builder.ins().iadd(left, right));
            }
            FNR_OP_CHECK_IF => {
                // Open a fresh block; the condition is computed on it and consumed by THEN.
                compiler.open_block(BlockKind::IfCond);
            }
            FNR_OP_THEN => {
                let frame = compiler.current_block_mut()?;
                ensure!(
                    matches!(frame.kind, BlockKind::IfCond),
                    "THEN without CHECK_IF"
                );
                ensure!(
                    frame.opstack.len() == 1,
                    "THEN expects exactly the condition on the opstack"
                );
                let cond = frame.opstack.pop().unwrap();

                let then_block = builder.create_block();
                let else_block = builder.create_block();
                builder.ins().brif(cond, then_block, &[], else_block, &[]);
                builder.switch_to_block(then_block);

                // Replace the condition block with the 'then' block.
                compiler.replace_block(BlockKind::IfThen { else_block })?;
            }
            FNR_OP_ELSE_THEN => {
                // The current block's opstack holds the then-branch results.
                let (results, else_block) = {
                    let frame = compiler.current_block_mut()?;
                    let else_block = match &frame.kind {
                        BlockKind::IfThen { else_block } => *else_block,
                        _ => bail!("ELSE_THEN without THEN"),
                    };
                    (std::mem::take(&mut frame.opstack), else_block)
                };

                let merge_block = builder.create_block();
                let mut merge_vars = Vec::with_capacity(results.len());
                for v in results {
                    ensure!(
                        builder.func.dfg.value_type(v) == types::I32,
                        "only 32-bit values can escape a branch"
                    );
                    let var = builder.declare_var(types::I32);
                    builder.def_var(var, v);
                    merge_vars.push(var);
                }
                builder.ins().jump(merge_block, &[]);
                builder.switch_to_block(else_block);

                // Replace the 'then' block with the 'else' block.
                compiler.replace_block(BlockKind::IfElse {
                    merge_block,
                    merge_vars,
                })?;
            }
            FNR_OP_END => {
                // The current block's opstack holds the else-branch results.
                let frame = compiler.close_block()?;
                let (merge_block, merge_vars) = match frame.kind {
                    BlockKind::IfElse {
                        merge_block,
                        merge_vars,
                    } => (merge_block, merge_vars),
                    _ => bail!("END without ELSE_THEN"),
                };
                let results = frame.opstack;
                ensure!(
                    results.len() == merge_vars.len(),
                    "if/else branches produce differing stacks"
                );
                for (&var, &v) in merge_vars.iter().zip(results.iter()) {
                    ensure!(
                        builder.func.dfg.value_type(v) == types::I32,
                        "only 32-bit values can escape a branch"
                    );
                    builder.def_var(var, v);
                }
                builder.ins().jump(merge_block, &[]);
                builder.switch_to_block(merge_block);

                // Push the merged branch results onto the enclosing block's opstack.
                let outer = compiler.current_block_mut()?;
                for &var in &merge_vars {
                    let v = builder.use_var(var);
                    outer.opstack.push(v);
                }
            }
            FNR_OP_INTRIN => {
                let nargs = r.read_uint()? as usize;
                let nrvs = r.read_uint()? as usize;
                let name = r.read_string()?;
                let (param_types, rvs) = intrinsic_signature(&name)
                    .ok_or_else(|| anyhow::anyhow!("unknown intrinsic: {name}"))?;
                ensure!(
                    nargs == param_types.len(),
                    "wrong number of arguments for intrinsic {name}"
                );
                ensure!(
                    nrvs == rvs,
                    "wrong number of return values for intrinsic {name}"
                );

                let mut args: Vec<Value> = Vec::with_capacity(nargs);
                {
                    let opstack = compiler.opstack_mut()?;
                    ensure!(opstack.len() >= nargs, "INTRIN with too few arguments");
                    for _ in 0..nargs {
                        args.push(opstack.pop().unwrap());
                    }
                }
                args.reverse();

                // Check operand widths against the kernel-side signature.
                let mut csig = Signature::new(CallConv::SystemV);
                for (&a, &ty) in args.iter().zip(param_types) {
                    ensure!(
                        builder.func.dfg.value_type(a) == ty,
                        "operand of wrong width for intrinsic {name}"
                    );
                    csig.params.push(AbiParam::new(ty));
                }
                for _ in 0..nrvs {
                    csig.returns.push(AbiParam::new(types::I32));
                }
                let sigref = builder.import_signature(csig);

                // Repeated calls to the same intrinsic share one external symbol and GOT slot.
                let index = externs.insert_full(name).0 as u32;
                let name_ref = builder
                    .func
                    .declare_imported_user_function(UserExternalName::new(0, index));
                let fref = builder.import_function(ExtFuncData {
                    name: ExternalName::user(name_ref),
                    signature: sigref,
                    colocated: false,
                });

                let call = builder.ins().call(fref, &args);
                let results: Vec<_> = builder.inst_results(call).to_vec();
                let opstack = compiler.opstack_mut()?;
                for v in results {
                    opstack.push(v);
                }
            }
            FNR_OP_SCOPE => {
                let count = r.read_uint()? as usize;
                // Move the top `count` operands off the current block's opstack.
                let opstack = compiler.opstack_mut()?;
                ensure!(
                    opstack.len() >= count,
                    "SCOPE moves more operands than available"
                );
                let start = opstack.len() - count;
                let moved: Vec<_> = opstack.drain(start..).collect();

                // The new scope inherits the enclosing scope's sstack and appends the moved operands.
                let mut sstack = compiler.current_sstack()?.clone();
                sstack.extend(moved);
                compiler.open_scope(sstack);
                compiler.open_block(BlockKind::Scope);
            }
            FNR_OP_SCOPE_END => {
                let frame = compiler.close_block()?;
                ensure!(
                    matches!(frame.kind, BlockKind::Scope),
                    "SCOPE_END without SCOPE"
                );
                compiler.close_scope();

                // Push the scope's results onto the enclosing block's opstack.
                let outer = compiler.current_block_mut()?;
                outer.opstack.extend(frame.opstack);
            }
            other => bail!("unexpected fafnir opcode: {other}"),
        }
    }

    let ret = compiler.return_value()?;
    ensure!(
        builder.func.dfg.value_type(ret) == types::I32,
        "kernlet must return a 32-bit value"
    );
    builder.ins().return_(&[ret]);

    builder.seal_all_blocks();
    builder.finalize();

    println!("kernletcc: Invoking Cranelift for compilation");
    let mut ctx = Context::for_function(func);

    // Extract the code and relocation descriptors into owned data,
    // releasing the borrow on `ctx` so that `ctx.func` can be inspected afterwards.
    let (code_bytes, raw_relocs) = {
        let compiled = ctx
            .compile(isa.as_ref(), &mut ControlPlane::default())
            .map_err(|e| anyhow::anyhow!("cranelift compilation failed: {:?}", e.inner))?;
        let code = compiled.code_buffer().to_vec();
        let raw: Vec<_> = compiled
            .buffer
            .relocs()
            .iter()
            .map(|r| (r.offset, r.addend, r.kind, r.target.clone()))
            .collect();
        (code, raw)
    };

    let mut relocs = Vec::new();
    for (offset, addend, kind, target) in raw_relocs {
        // We only emit calls to imported intrinsics, which in PIC mode become GOT-relative
        // loads of the symbol address against external user symbols.
        ensure!(
            matches!(kind, Reloc::X86GOTPCRel4),
            "unexpected relocation kind: {kind:?}"
        );
        let symbol = match target {
            FinalizedRelocTarget::ExternalName(ExternalName::User(name_ref)) => {
                let uen = &ctx.func.params.user_named_funcs()[name_ref];
                ensure!(
                    (uen.index as usize) < externs.len(),
                    "relocation references unknown intrinsic"
                );
                uen.index
            }
            other => bail!("unexpected relocation target: {other:?}"),
        };
        relocs.push(ExternReloc {
            offset,
            addend,
            symbol,
        });
    }

    // The single compiled function is the entry point `automate_irq`, which the kernel loader
    // resolves by name (kernel/thor/generic/kernlet.cpp).
    let code_size = code_bytes.len() as u32;
    let compiled = Compiled {
        code: code_bytes,
        exports: vec![Export {
            name: "automate_irq".to_string(),
            offset: 0,
            size: code_size,
        }],
        externs: externs.into_iter().collect(),
        relocs,
    };

    println!("kernletcc: Compilation via Cranelift completed");
    Ok(compiled)
}
