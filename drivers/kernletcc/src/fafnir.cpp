
#include <stdint.h>
#include <iostream>
#include <vector>
#include <fafnir/language.h>
#include <lewis/elf/object.hpp>
#include <lewis/elf/passes.hpp>
#include <lewis/elf/file-emitter.hpp>
#include <lewis/target-x86_64/arch-passes.hpp>
#include <lewis/target-x86_64/mc-emitter.hpp>

struct Compilation {
	lewis::Function fn;
	lewis::BasicBlock *bb;
	std::vector<lewis::Value *> opstack;
};

std::vector<uint8_t> compileFafnir(const uint8_t *code, size_t size) {
	Compilation comp;
	comp.fn.name = "automate_irq";
	comp.bb = comp.fn.addBlock(std::make_unique<lewis::BasicBlock>());

	auto s = code;

	auto extractUint = [&] () -> unsigned int {
		assert(s < code + size);
		unsigned int x = *s;
		s++;
		return x;
	};

	auto extractString = [&] () -> std::string {
		std::string str;
		while(true) {
			assert(s < code + size);
			auto c = static_cast<char>(*s);
			s++;
			if(!c)
				break;
			str += c;
		}
		return str;
	};

	auto phi = comp.bb->attachPhi(std::make_unique<lewis::ArgumentPhi>());
	auto argument = phi->value.setNew<lewis::LocalValue>();
	argument->setType(lewis::globalPointerType());

	while(s < code + size) {
		auto opcode = extractUint();
		if(opcode == FNR_OP_CONST) {
			auto operand = extractUint();

			auto inst = comp.bb->insertNewInstruction<lewis::LoadConstInstruction>(operand);
			auto result = inst->result.setNew<lewis::LocalValue>();
			result->setType(lewis::globalInt32Type());
			comp.opstack.push_back(result);
		}else if(opcode == FNR_OP_BINDING) {
			auto index = extractUint();

			if(index == 0) {
				auto inst = comp.bb->insertNewInstruction<lewis::LoadOffsetInstruction>(
						argument, 0);
				auto result = inst->result.setNew<lewis::LocalValue>();
				result->setType(lewis::globalPointerType());
				comp.opstack.push_back(result);
			}else if(index == 1) {
				auto inst = comp.bb->insertNewInstruction<lewis::LoadOffsetInstruction>(
						argument, 8);
				auto result = inst->result.setNew<lewis::LocalValue>();
				result->setType(lewis::globalInt32Type());
				comp.opstack.push_back(result);
			}else assert(!"TODO: Implement generic bindings");
		}else if(opcode == FNR_OP_AND) {
			auto left = comp.opstack.back();
			comp.opstack.pop_back();
			auto right = comp.opstack.back();
			comp.opstack.pop_back();

			auto inst = comp.bb->insertNewInstruction<lewis::BinaryMathInstruction>(
					lewis::BinaryMathOpcode::bitwiseAnd, left, right);
			auto result = inst->result.setNew<lewis::LocalValue>();
			result->setType(lewis::globalInt32Type());
			comp.opstack.push_back(result);
		}else if(opcode == FNR_OP_ADD) {
			auto left = comp.opstack.back();
			comp.opstack.pop_back();
			auto right = comp.opstack.back();
			comp.opstack.pop_back();

			auto inst = comp.bb->insertNewInstruction<lewis::BinaryMathInstruction>(
					lewis::BinaryMathOpcode::add, left, right);
			auto result = inst->result.setNew<lewis::LocalValue>();
			result->setType(lewis::globalInt32Type());
			comp.opstack.push_back(result);
		}else if(opcode == FNR_OP_INTRIN) {
			auto function = extractString();

			auto handle = comp.opstack.back();
			comp.opstack.pop_back();
			auto offset = comp.opstack.back();
			comp.opstack.pop_back();

			auto inst = comp.bb->insertNewInstruction<lewis::InvokeInstruction>(
					std::move(function), 2);
			inst->operand(0) = handle;
			inst->operand(1) = offset;
			auto result = inst->result.setNew<lewis::LocalValue>();
			result->setType(lewis::globalInt32Type());
			comp.opstack.push_back(result);
		}else{
			std::cerr << "FNR opcode: " << opcode << std::endl;
			assert(!"Unexpected fafnir opcode");
		}
	}

    auto branch = comp.bb->setBranch(std::make_unique<lewis::FunctionReturnBranch>(1));
	branch->operand(0) = comp.opstack.back();
	comp.opstack.pop_back();
	assert(comp.opstack.empty());

	// Lower to x86_64 and emit machine code.
	std::cout << "kernletcc: Invoking lewis for compilation" << std::endl;
	for(auto bb : comp.fn.blocks()) {
		auto lower = lewis::targets::x86_64::LowerCodePass::create(bb);
		lower->run();
	}
    auto ra = lewis::targets::x86_64::AllocateRegistersPass::create(&comp.fn);
    ra->run();

    lewis::elf::Object elf;
    lewis::targets::x86_64::MachineCodeEmitter mce{&comp.fn, &elf};
    mce.run();

    // Create headers and layout the file.
    auto headers_pass = lewis::elf::CreateHeadersPass::create(&elf);
    auto layout_pass = lewis::elf::LayoutPass::create(&elf);
    auto link_pass = lewis::elf::InternalLinkPass::create(&elf);
    headers_pass->run();
    layout_pass->run();
    link_pass->run();

    // Compose the output file.
    auto file_emitter = lewis::elf::FileEmitter::create(&elf);
    file_emitter->run();
	std::cout << "kernletcc: Compilation via lewis completed" << std::endl;
	return file_emitter->buffer;
}

