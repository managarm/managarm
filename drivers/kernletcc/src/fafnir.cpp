
#include <stdint.h>
#include <iostream>
#include <vector>
#include <fafnir/language.h>
#include <lewis/elf/object.hpp>
#include <lewis/elf/passes.hpp>
#include <lewis/elf/file-emitter.hpp>
#include <lewis/target-x86_64/arch-passes.hpp>
#include <lewis/target-x86_64/mc-emitter.hpp>
#include "common.hpp"

struct Binding {
	BindType type;
	size_t disp;
};

struct Scope {
	lewis::BasicBlock *insertBb = nullptr;
	lewis::Value *instance = nullptr;
	std::vector<lewis::Value *> sstack;
};

struct Ite {
	Scope *ifScope = nullptr;
	Scope *elseScope = nullptr;
};

struct Compilation {
	std::vector<Binding> bindings;

	lewis::Function fn;
	std::vector<lewis::Value *> opstack;
	std::vector<Scope *> activeScopes;
	std::vector<Ite> activeBlocks;
};

std::vector<uint8_t> compileFafnir(const uint8_t *code, size_t size,
		const std::vector<BindType> &bind_types) {
	Compilation compilation;
	Compilation *comp = &compilation;

	auto initial_scope = new Scope;
	initial_scope->insertBb = comp->fn.addBlock(std::make_unique<lewis::BasicBlock>());
	comp->activeScopes.push_back(initial_scope);

	auto argument_phi = initial_scope->insertBb->attachPhi(std::make_unique<lewis::ArgumentPhi>());
	initial_scope->instance = argument_phi->value.setNew<lewis::LocalValue>();
	initial_scope->instance->setType(lewis::globalPointerType());

	size_t sizeof_bindings = 0;
	for(auto bt : bind_types) {
		comp->bindings.push_back({bt, sizeof_bindings});
		sizeof_bindings += 8;
	}

	comp->fn.name = "automate_irq";

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

	while(s < code + size) {
		assert(!comp->activeScopes.empty());
		Scope *scope = comp->activeScopes.back();

		auto opcode = extractUint();
		switch(opcode) {
		case FNR_OP_DUP: {
			auto index = extractUint();
			assert(comp->opstack.size() > index);
			comp->opstack.push_back(comp->opstack[comp->opstack.size() - index - 1]);
			break;
		}
		case FNR_OP_DROP: {
			comp->opstack.pop_back();
			break;
		}
		case FNR_OP_LITERAL: {
			auto operand = extractUint();

			auto inst = scope->insertBb->insertNewInstruction<lewis::LoadConstInstruction>(operand);
			auto result = inst->result.setNew<lewis::LocalValue>();
			result->setType(lewis::globalInt32Type());
			comp->opstack.push_back(result);
			break;
		}
		case FNR_OP_BINDING: {
			auto index = extractUint();
			assert(index < comp->bindings.size());

			switch(comp->bindings[index].type) {
			case BindType::offset: {
				auto inst = scope->insertBb->insertNewInstruction<lewis::LoadOffsetInstruction>(
						scope->instance, comp->bindings[index].disp);
				auto result = inst->result.setNew<lewis::LocalValue>();
				result->setType(lewis::globalInt32Type());
				comp->opstack.push_back(result);
				break;
			}
			case BindType::memoryView: {
				auto inst = scope->insertBb->insertNewInstruction<lewis::LoadOffsetInstruction>(
						scope->instance, comp->bindings[index].disp);
				auto result = inst->result.setNew<lewis::LocalValue>();
				result->setType(lewis::globalPointerType());
				comp->opstack.push_back(result);
				break;
			}
			case BindType::bitsetEvent: {
				auto inst = scope->insertBb->insertNewInstruction<lewis::LoadOffsetInstruction>(
						scope->instance, comp->bindings[index].disp);
				auto result = inst->result.setNew<lewis::LocalValue>();
				result->setType(lewis::globalPointerType());
				comp->opstack.push_back(result);
				break;
			}
			default:
				assert(!"Unexpected binding type");
			}

			break;
		}
		case FNR_OP_S_DEFINE: {
			assert(comp->opstack.size());
			auto operand = comp->opstack.back();
			comp->opstack.pop_back();

			scope->sstack.push_back(operand);
			break;
		}
		case FNR_OP_S_VALUE: {
			auto index = extractUint();
			assert(index < scope->sstack.size());

			comp->opstack.push_back(scope->sstack[index]);
			break;
		}
		case FNR_OP_CHECK_IF: {
			assert(comp->opstack.empty());
			comp->activeBlocks.push_back(Ite{});
			break;
		}
		case FNR_OP_THEN: {
			assert(comp->opstack.size() == 1);
			assert(!comp->activeBlocks.empty());

			// Add a jump to the current BB.
			auto outer = scope;
			auto operand = comp->opstack.back();
			comp->opstack.pop_back();

			auto branch = outer->insertBb->setBranch(std::make_unique<lewis::ConditionalBranch>());
			branch->operand = operand;

			// Setup the scope with a new BB.
			auto inner = new Scope;
			inner->insertBb = comp->fn.addBlock(std::make_unique<lewis::BasicBlock>());
			branch->ifTarget = inner->insertBb;

			// Setup the instance value.
			auto instance_phi = inner->insertBb->attachPhi(std::make_unique<lewis::DataFlowPhi>());
			auto instance_edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
					outer->insertBb->source, instance_phi->sink);
			instance_edge->alias = outer->instance;

			inner->instance = instance_phi->value.setNew<lewis::LocalValue>();
			inner->instance->setType(lewis::globalPointerType());

			// Setup the sstack values.
			for(size_t i = 0; i < outer->sstack.size(); i++) {
				auto phi = inner->insertBb->attachPhi(std::make_unique<lewis::DataFlowPhi>());
				auto edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
						outer->insertBb->source, phi->sink);
				edge->alias = outer->sstack[i];

				auto value = phi->value.setNew<lewis::LocalValue>();
				value->setType(lewis::globalInt32Type());
				inner->sstack.push_back(value);
			}

			comp->activeBlocks.back().ifScope = inner;
			comp->activeScopes.push_back(inner);
			break;
		}
		case FNR_OP_ELSE_THEN: {
			assert(comp->opstack.empty());
			assert(!comp->activeBlocks.empty());
			assert(comp->activeScopes.size() >= 2);

			// The previous scope ends here. It is still accessible via the Ite struct.
			comp->activeScopes.pop_back();
			auto outer = comp->activeScopes.back();
			auto branch = lewis::hierarchy_cast<lewis::ConditionalBranch *>(outer->insertBb->branch());

			// Setup the scope with a new BB.
			auto inner = new Scope;
			inner->insertBb = comp->fn.addBlock(std::make_unique<lewis::BasicBlock>());
			branch->elseTarget = inner->insertBb;

			// Setup the instance value.
			auto instance_phi = inner->insertBb->attachPhi(std::make_unique<lewis::DataFlowPhi>());
			auto instance_edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
					outer->insertBb->source, instance_phi->sink);
			instance_edge->alias = outer->instance;

			inner->instance = instance_phi->value.setNew<lewis::LocalValue>();
			inner->instance->setType(lewis::globalPointerType());

			// Setup the sstack values.
			for(size_t i = 0; i < outer->sstack.size(); i++) {
				auto phi = inner->insertBb->attachPhi(std::make_unique<lewis::DataFlowPhi>());
				auto edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
						outer->insertBb->source, phi->sink);
				edge->alias = outer->sstack[i];

				auto value = phi->value.setNew<lewis::LocalValue>();
				value->setType(lewis::globalInt32Type());
				inner->sstack.push_back(value);
			}

			comp->activeBlocks.back().elseScope = inner;
			comp->activeScopes.push_back(inner);
			break;
		}
		case FNR_OP_END: {
			assert(comp->opstack.empty());
			assert(!comp->activeBlocks.empty());
			assert(comp->activeScopes.size() >= 2);

			// The previous scope ends here. It is still accessible via the Ite struct.
			comp->activeScopes.pop_back();
			auto ite = &comp->activeBlocks.back();
			auto outer = comp->activeScopes.back();

			// Set up a new BB for the existing scope.
			outer->insertBb = comp->fn.addBlock(std::make_unique<lewis::BasicBlock>());
			ite->ifScope->insertBb->setBranch(std::make_unique<lewis::UnconditionalBranch>(outer->insertBb));
			ite->elseScope->insertBb->setBranch(std::make_unique<lewis::UnconditionalBranch>(outer->insertBb));

			// Reset the instance value.
			auto instance_phi = outer->insertBb->attachPhi(std::make_unique<lewis::DataFlowPhi>());
			auto instance_if_edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
					ite->ifScope->insertBb->source, instance_phi->sink);
			auto instance_else_edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
					ite->elseScope->insertBb->source, instance_phi->sink);
			instance_if_edge->alias = ite->ifScope->instance;
			instance_else_edge->alias = ite->elseScope->instance;

			outer->instance = instance_phi->value.setNew<lewis::LocalValue>();
			outer->instance->setType(lewis::globalPointerType());

			// Reset all sstack values.
			for(size_t i = 0; i < outer->sstack.size(); i++) {
				auto phi = outer->insertBb->attachPhi(std::make_unique<lewis::DataFlowPhi>());
				auto if_edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
						ite->ifScope->insertBb->source, phi->sink);
				auto else_edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
						ite->elseScope->insertBb->source, phi->sink);
				if_edge->alias = ite->ifScope->sstack[i];
				else_edge->alias = ite->elseScope->sstack[i];

				auto value = phi->value.setNew<lewis::LocalValue>();
				value->setType(lewis::globalInt32Type());
				outer->sstack[i] = value;
			}

			auto n = ite->ifScope->sstack.size();
			assert(n == ite->elseScope->sstack.size());
			assert(n >= outer->sstack.size());

			// Push items from the inner sstack to the opstack.
			for(size_t i = outer->sstack.size(); i < n; i++) {
				auto phi = outer->insertBb->attachPhi(std::make_unique<lewis::DataFlowPhi>());
				auto if_edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
						ite->ifScope->insertBb->source, phi->sink);
				auto else_edge = lewis::DataFlowEdge::attach(std::make_unique<lewis::DataFlowEdge>(),
						ite->elseScope->insertBb->source, phi->sink);
				if_edge->alias = ite->ifScope->sstack[i];
				else_edge->alias = ite->elseScope->sstack[i];

				auto value = phi->value.setNew<lewis::LocalValue>();
				value->setType(lewis::globalInt32Type());
				comp->opstack.push_back(value);
			}

			comp->activeBlocks.pop_back();
			break;
		}
		case FNR_OP_BITWISE_AND: {
			assert(comp->opstack.size() >= 2);
			auto right = comp->opstack.back();
			comp->opstack.pop_back();
			auto left = comp->opstack.back();
			comp->opstack.pop_back();

			auto inst = scope->insertBb->insertNewInstruction<lewis::BinaryMathInstruction>(
					lewis::BinaryMathOpcode::bitwiseAnd, left, right);
			auto result = inst->result.setNew<lewis::LocalValue>();
			result->setType(lewis::globalInt32Type());
			comp->opstack.push_back(result);
			break;
		}
		case FNR_OP_ADD: {
			assert(comp->opstack.size() >= 2);
			auto right = comp->opstack.back();
			comp->opstack.pop_back();
			auto left = comp->opstack.back();
			comp->opstack.pop_back();

			auto inst = scope->insertBb->insertNewInstruction<lewis::BinaryMathInstruction>(
					lewis::BinaryMathOpcode::add, left, right);
			auto result = inst->result.setNew<lewis::LocalValue>();
			result->setType(lewis::globalInt32Type());
			comp->opstack.push_back(result);
			break;
		}
		case FNR_OP_INTRIN: {
			int nargs = extractUint();
			int nrvs = extractUint();
			auto function = extractString();
			assert(comp->opstack.size() >= static_cast<size_t>(nargs));

			auto inst = scope->insertBb->insertNewInstruction<lewis::InvokeInstruction>(
					std::move(function), nargs, nrvs);
			for(int i = nargs - 1; i >= 0; i--) {
				inst->operand(i) = comp->opstack.back();
				comp->opstack.pop_back();
			}

			for(int i = 0; i < nrvs; i++) {
				auto result = inst->result(i).setNew<lewis::LocalValue>();
				result->setType(lewis::globalInt32Type());
				comp->opstack.push_back(result);
			}

			break;
		}
		default:
			std::cerr << "FNR opcode: " << opcode << std::endl;
			assert(!"Unexpected fafnir opcode");
		}
	}

	assert(!comp->activeScopes.empty());
	Scope *final_scope = comp->activeScopes.back();

	assert(comp->opstack.size() == 1);
    auto branch = final_scope->insertBb->setBranch(std::make_unique<lewis::FunctionReturnBranch>(1));
	branch->operand(0) = comp->opstack.back();
	comp->opstack.pop_back();
	assert(comp->opstack.empty());

	// Lower to x86_64 and emit machine code.
	std::cout << "kernletcc: Invoking lewis for compilation" << std::endl;
	for(auto bb : comp->fn.blocks()) {
		auto lower = lewis::targets::x86_64::LowerCodePass::create(bb);
		lower->run();
	}
    auto ra = lewis::targets::x86_64::AllocateRegistersPass::create(&comp->fn);
    ra->run();

    lewis::elf::Object elf;
    lewis::targets::x86_64::MachineCodeEmitter mce{&comp->fn, &elf};
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

