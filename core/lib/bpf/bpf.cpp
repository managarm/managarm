#include <arch/bit.hpp>
#include <core/bpf.hpp>

bool Bpf::validate() {
	for (size_t pc = 0; pc < prog_.size(); pc++) {
		auto inst = prog_[pc];

		switch (Op(inst.code)) {
		case Op::JMP_JEQ_K:
		// case Op::JMP_JEQ_X:
		// case Op::JMP_JGE_K:
		// case Op::JMP_JGE_X:
		// case Op::JMP_JGT_K:
		// case Op::JMP_JGT_X:
		case Op::JMP_JSET_K:
			// case Op::JMP_JSET_X:
			if (pc + inst.jt + 1 >= prog_.size() || pc + inst.jf + 1 >= prog_.size())
				return false;
			break;
		default:
			break;
		}
	}

	Op last = Op(prog_.back().code);
	if (last != Op::RET_K && last != Op::RET_A)
		return false;

	return true;
}

uint32_t Bpf::run(arch::dma_buffer_view buffer) {
	size_t pc = 0;

	auto bpf_log_op = [&pc, this](const char *format, auto... args) {
		if (logBpfOps) {
			printf("\t[%.2zu/%.2zu] ", pc, prog_.size() - 1);
			printf(format, args...);
			puts("");
		}
	};

	for (pc = 0; pc < prog_.size(); pc++) {
		auto inst = prog_[pc];

		auto load = [&buffer]<typename T>(size_t offset) -> T {
			if (!(offset + sizeof(T) <= buffer.size())) {
				printf(
				    "core/bpf: read of size 0x%zx at offset 0x%zx would be out of bounds (buffer "
				    "size 0x%zx)",
				    sizeof(T),
				    offset,
				    buffer.size()
				);
				assert(offset + sizeof(T) <= buffer.size());
			}
			return arch::convert_endian<arch::endian::big>(
			    *reinterpret_cast<T *>(buffer.subview(offset, sizeof(T)).data())
			);
		};

		switch (Op(inst.code)) {
		case Op::ALU_ADD_X: {
			bpf_log_op("A (0x%x) += X (0x%x) = 0x%x", A, X, A * X);
			A += X;
			break;
		}
		case Op::ALU_AND_K: {
			bpf_log_op("A (0x%x) &= k (0x%x) = 0x%x", A, inst.k, A & inst.k);
			A &= inst.k;
			break;
		}
		case Op::ALU_MUL_K: {
			bpf_log_op("A (0x%x) *= k (0x%x) = 0x%x", A, inst.k, A * inst.k);
			A *= inst.k;
			break;
		}
		case Op::JMP_JEQ_K: {
			bpf_log_op(
			    "PC += 0x%02x if A == K (0x%x == 0x%x) else 0x%02x (0x%02x)",
			    inst.jt,
			    A,
			    inst.k,
			    inst.jf,
			    (A == inst.k) ? inst.jt : inst.jf
			);
			pc += (A == inst.k) ? inst.jt : inst.jf;
			break;
		}
		case Op::JMP_JSET_K: {
			bpf_log_op(
			    "PC += 0x%02x if A & k (0x%x & 0x%x) else 0x%02x (0x%02x)",
			    inst.jt,
			    A,
			    inst.k,
			    inst.jf,
			    (A & inst.k) ? inst.jt : inst.jf
			);
			pc += (A & inst.k) ? inst.jt : inst.jf;
			break;
		}
		case Op::LDX_W_IMM: {
			bpf_log_op("X <- k (0x%02x)", inst.k);
			X = inst.k;
			break;
		}
		case Op::LD_B_IND: {
			auto val = load.template operator()<uint8_t>(X + inst.k);
			bpf_log_op("A <- P[X+k:1 (0x%02x + 0x%02x)] (0x%hx)", X, inst.k, val);
			A = val;
			break;
		}
		case Op::LD_H_ABS: {
			auto val = load.template operator()<uint16_t>(inst.k);
			bpf_log_op("A <- P[k:2 (0x%02x)] = 0x%hx", inst.k, val);
			A = val;
			break;
		}
		case Op::LD_H_IND: {
			auto val = load.template operator()<uint16_t>(X + inst.k);
			bpf_log_op("A <- P[X+k:2 (0x%02x + 0x%02x)] (0x%hx)", X, inst.k, val);
			A = val;
			break;
		}
		case Op::LD_W_ABS: {
			auto val = load.template operator()<uint32_t>(inst.k);
			bpf_log_op("A <- P[k:4 (0x%04x)] = 0x%x", inst.k, val);
			A = val;
			break;
		}
		case Op::LD_W_IND: {
			auto val = load.template operator()<uint32_t>(X + inst.k);
			bpf_log_op("A <- P[X+k:4 (0x%02x + 0x%02x)] (0x%hx)", X, inst.k, val);
			A = val;
			break;
		}
		case Op::MISC_TAX: {
			bpf_log_op("X <- A (0x%02x)", A);
			X = A;
			break;
		}
		case Op::RET_K: {
			bpf_log_op("RET k (0x%02x)", inst.k);
			return inst.k;
		}
		default:
			// TODO: for now, an unknown BPF instruction is a hard failure as our coverage of
			// the instruction set is quite incomplete. In the future, once that doesn't hold
			// any more, we should do proper error handling here instead (e.g. return EINVAL).
			printf("core: unhandled BPF instruction 0x%02x\n", inst.code);
			bpf_log_op("{ 0x%02x, %.2u, %.2u, 0x%08x }\n", inst.code, inst.jt, inst.jf, inst.k);
			assert(!"unhandled BPF instruction");
		}
	}

	assert(!"invalid BPF filter with no return!");
}
