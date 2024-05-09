#pragma once

#include <arch/dma_pool.hpp>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <linux/filter.h>
#include <span>

constexpr bool logBpfOps = false;

struct Bpf {
	enum class Op : uint16_t {
		ALU_ADD_X = BPF_ALU | BPF_ADD | BPF_X,
		ALU_AND_K = BPF_ALU | BPF_AND | BPF_K,
		ALU_MUL_K = BPF_ALU | BPF_MUL | BPF_K,
		JMP_JEQ_K = BPF_JMP | BPF_JEQ | BPF_K,
		JMP_JSET_K = BPF_JMP | BPF_JSET | BPF_K,
		LDX_W_IMM = BPF_LDX | BPF_W | BPF_IMM,
		LD_B_IND = BPF_LD | BPF_B | BPF_IND,
		LD_H_ABS = BPF_LD | BPF_H | BPF_ABS,
		LD_H_IND = BPF_LD | BPF_H | BPF_IND,
		LD_W_IND = BPF_LD | BPF_W | BPF_IND,
		MISC_TAX = BPF_MISC | BPF_TAX,
		RET_A = BPF_RET | BPF_A,
		RET_K = BPF_RET | BPF_K,
	};

	Bpf(std::span<char> fprog)
		: prog_{
			reinterpret_cast<struct sock_filter *>(fprog.data()),
			fprog.size() / sizeof(struct sock_filter)
		} {

	}

	bool validate();

	uint32_t run(arch::dma_buffer_view buffer);
private:
	std::span<struct sock_filter> prog_;

	// accumulator register
	uint32_t A = 0;
	// index register
	uint32_t X = 0;
};
